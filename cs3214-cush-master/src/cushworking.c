/*
 * cush - the customizable shell.
 *
 * Authors: Naud Tafesse
 *
 * Developed by Godmar Back for CS 3214 Summer 2020
 * Virginia Tech. Augmented to use posix_spawn in Fall 2021.
 */

#define _GNU_SOURCE 1
#include <stdio.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/wait.h>
#include <assert.h>
#include <fcntl.h>

/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "termstate_management.h"
#include "signal_support.h"
#include "shell-ast.h"
#include "utils.h"
#include "spawn.h"

#define MAXJOBS (1<<16)
#define PIPE_READ (0)
#define PIPE_WRITE (1)

static void handle_child_status(pid_t pid, int status);

static void
usage(char *progname)
{
    printf("Usage: %s -h\n"
        " -h            print this help\n", progname);  
    exit(EXIT_SUCCESS);
}

/* Build a prompt */
static char *
build_prompt(void)
{
    return strdup("cush> ");
}

enum job_status {
    FOREGROUND,   /* job is running in foreground. Only one job can be in the foreground state. */
    BACKGROUND,   /* job is running in background */
    STOPPED,      /* job is stopped via SIGSTOP */
    NEEDSTERMINAL,/* job is stopped because it was a background job and requires exclusive terminal access */
    DONE,         /* job is finished running */
};

struct job {
    struct list_elem elem;   /* Link element for jobs list. */
    struct ast_pipeline *pipe; /* The pipeline of commands this job represents */
    int jid;                 /* Job id. */
    enum job_status status;  /* Job status. */
    int num_processes_alive; /* The number of processes that we know to be alive */
    struct termios saved_tty_state; /* The state of the terminal when this job was stopped after having been in foreground */
    pid_t *pids;             /* Add additional fields here if needed. */
};

/* Utility functions for job list management.
 * We use 2 data structures:
 * (a) an array jid2job to quickly find a job based on its id
 * (b) a linked list to support iteration
 */

static struct list job_list;
static struct job *jid2job[MAXJOBS];

/* Return job corresponding to jid */
static struct job *
get_job_from_jid(int jid)
{
    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
        return jid2job[jid];

    return NULL;
}
static int findEmptyPIDSlot(struct job* theJob) {
    for (int i = 0; i < MAXJOBS; i++) {
        if (theJob->pids[i] == 0) {
            return i;
        }
    }

    printf("THE INDEX RETURNED IS -1\n");
    return -1;
}

/* Add a new job to the job list */
static struct job*
add_job(struct ast_pipeline *pipe)
{
    struct job *job = calloc(1, sizeof *job); // MODIFIED TO USE CALLOC TO PREVENT VALGRIND ERRORS (originally malloc)
    job->pipe = pipe;
    job->num_processes_alive = 0;
    job->pids = (pid_t*) calloc(MAXJOBS, sizeof(pid_t)); // CALLOC FOR PID ARRAY
    list_push_back(&job_list, &job->elem);
    for (int i = 1; i < MAXJOBS; i++) {
        if (jid2job[i] == NULL) {
            jid2job[i] = job;
            job->jid = i;
            return job;
        }
    }
    fprintf(stderr, "Maximum number of jobs exceeded\n");
    abort();
    return NULL;
}

/* Delete a job.
 * This should be called only when all processes that were forked for this job are known to have terminated.
 */
static void
delete_job(struct job *job)
{
    int jid = job->jid;
    assert(jid != -1);
    jid2job[jid]->jid = -1;
    jid2job[jid] = NULL;
    ast_pipeline_free(job->pipe);
    free(job->pids);
    free(job);
}
static const char *
get_status(enum job_status status)
{
    switch (status) {
    case FOREGROUND:
        return "Foreground";
    case BACKGROUND:
        return "Running";
    case STOPPED:
        return "Stopped";
    case NEEDSTERMINAL:
        return "Stopped (tty)";
    case DONE:
        return "Done";
    default:
        return "Unknown";
    }
}

/* Print the command line that belongs to one job. */
static void
print_cmdline(struct ast_pipeline *pipeline)
{
    struct list_elem *e = list_begin(&pipeline->commands);
    for (; e != list_end(&pipeline->commands); e = list_next(e)) {
        struct ast_command *cmd = list_entry(e, struct ast_command, elem);
        if (e != list_begin(&pipeline->commands))
            printf("| ");
        char **p = cmd->argv;
        printf("%s", *p++);
        while (*p)
            printf(" %s", *p++);
    }
}
static void
print_job(struct job *job)
{
    printf("[%d]\t%s\t\t", job->jid, get_status(job->status));
    print_cmdline(job->pipe);
    printf(")\n");
}

/*
 * Suggested SIGCHLD handler.
 *
 * Call waitpid() to learn about any child processes that
 * have exited or changed status (been stopped, needed the
 * terminal, etc.)
 * Just record the information by updating the job list
 * data structures. Since the call may be spurious (e.g.
 * an already pending SIGCHLD is delivered even though
 * a foreground process was already reaped), ignore when
 * waitpid returns -1.
 * Use a loop with WNOHANG since only a single SIGCHLD
 * signal may be delivered for multiple children that have
 * exited. All of them need to be reaped.
 */

static void
sigchld_handler(int sig, siginfo_t *info, void *_ctxt)
{
    pid_t child;
    int status;

    assert(sig == SIGCHLD);

    while ((child = waitpid(-1, &status, WUNTRACED|WNOHANG)) > 0) {
        handle_child_status(child, status);
    }
}
/* Wait for all processes in this job to complete, or for
 * the job no longer to be in the foreground.
 * You should call this function from a) where you wait for
 * jobs started without the &; and b) where you implement the
 * fg command.
 *
 * Implement handle_child_status such that it records the
 * information obtained from waitpid() for pid 'child.'
 *
 * If a process exited, it must find the job to which it
 * belongs and decrement num_processes_alive.
 *
 * However, note that it is not safe to call delete_job
 */
static void
wait_for_job(struct job *job)
{
    assert(signal_is_blocked(SIGCHLD));

    while (job->status == FOREGROUND && job->num_processes_alive > 0) {
        int status;
        pid_t child = waitpid(-1, &status, WUNTRACED);

        // When called here, any error returned by waitpid indicates a logic
        // bug in the shell.
        // In particular, ECHILD "No child process" means that there has
        // already been a successful waitpid() call that reaped the child, so
        // there's likely a bug in handle_child_status where it failed to update
        // the "job" status and/or num_processes_alive fields in the required
        // fashion.
        // Since SIGCHLD is blocked, there cannot be races where a child's exit
        // was handled via the SIGCHLD signal handler.
        if (child != -1)
            handle_child_status(child, status);
        else
            utils_fatal_error("waitpid failed, see code for explanation");
    }
}

static void
handle_child_status(pid_t pid, int status)
{
    assert(signal_is_blocked(SIGCHLD));

    /* To be implemented.
     * Step 1. Given the pid, determine which job this pid is a part of
     * (how to do this is not part of the provided code.)
     * Step 2. Determine what status change occurred using the
     * WIF*() macros.
     * Step 3. Update the job status accordingly, and adjust
     * num_processes_alive if appropriate.
     *
     * If a process was stopped, save the terminal state.
     */

    // Step 1
    struct job* j = NULL;

    for (struct list_elem* e = list_begin(&job_list); e != list_end(&job_list); e = list_next(e)) {
        struct job* tempJob = list_entry(e, struct job, elem);

        for (int i = 0; i < MAXJOBS; i++) {
            if (tempJob->pids[i] == pid) {
                j = tempJob;
                break;
            }
        }
    }

    if (j == NULL) {
        printf("HANDLE CHILD STATUS: JOB IS NULL\n");
        return;
    }

    // Step 2 and 3
    if (WIFEXITED(status)) {
        j->num_processes_alive--;
        if (j->status == FOREGROUND && status == 0) {
            termstate_sample();
        }
        else if (j->status == BACKGROUND) {
        j->status = DONE;
        print_job(j);
        }
    } 
    else if (WIFSIGNALED(status)) {
        j->num_processes_alive--;

        if (j->status == BACKGROUND) {
            j->status = DONE;
        }

        int signal = WTERMSIG(status);
        printf(strsignal(signal)); // Prints out what might have terminated the process
    } 
    else if (WIFSTOPPED(status)) {
        if (j->status == FOREGROUND) {
            termstate_save(&j->saved_tty_state);
        }

        j->status = STOPPED;

        if (WSTOPSIG(status) == SIGTSTP) {
            print_job(j);
        }
    }
}
/*
 * Removes the jobs that are in the DONE status; used at the very end
 * after processing each job
 */
static void removeDoneJobs() {
    int counter = 0;
    int i = 0;

    while (counter < list_size(&job_list) && i < MAXJOBS) {
        struct job* tempJob = jid2job[i];
        if (tempJob != NULL) {
            if (tempJob->status == DONE) {
                list_remove(&tempJob->elem);
                delete_job(tempJob);
            }
            counter++
        }
        i++;
    }
}
/*
 * This method handles printing out the commands used in the history
 * of the shell
 */
static void helper_history() {
    HIST_ENTRY** the_list = history_list();

    for (int i = 0; i < history_length; i++) {
        printf("%d: %s\n", i + history_base, the_list[i]->line);
    }
}

/*
 * This method handles finding the designated folder given the path
 */
static void helper_cd(char *path) {
    char* home;
    if (path == NULL || path[0] == '\0' || strcmp(path, "~") == 0) {
        home = getenv("HOME");
        if (home == NULL) {
            printf("cd: Unable to determine HOME directory\n");
            return;
        }
        path = home;
    }
    if (chdir(path) != 0) {
        perror("cd");
    }
}
/*
 * Helper method to handle the fg command
 */
static void helper_fg(char* theJid) {
    if (theJid == NULL) {
        printf("fg: current: no such job\n");
        return;
    }

    int jid = atoi(theJid);
    struct job* theJob = get_job_from_jid(jid);

    if (theJob == NULL) {
        printf("fg: %d: no such job\n", jid);
        return;
    }

    // call tcsetattr and tcsetpgrp
    tcsetattr(termstate_get_tty_fd(), TCSANOW, &theJob->saved_tty_state);
    tcsetpgrp(termstate_get_tty_fd(), theJob->pids[0]);

    print_cmdline(theJob->pipe);
    printf("\n");

    int status = killpg(theJob->pids[0], SIGCONT);

    if (status == 0) {
        theJob->status = FOREGROUND;
        termstate_give_terminal_to(&theJob->saved_tty_state, theJob->pids[0]);
        wait_for_job(theJob);

        if (theJob->status == FOREGROUND) {
            if (status == 0) {
                if (theJob->status == FOREGROUND) {
                    list_remove(&theJob->elem);
                    delete_job(theJob);
                }
            }
        }
    }
}
/*
 * Helper method to handle the bg command
 */
static void helper_bg(char* theJid) {
    if (theJid == NULL) {
        printf("bg: current: no such job\n");
        return;
    }

    int jid = atoi(theJid);
    struct job* theJob = get_job_from_jid(jid);

    if (theJob == NULL) {
        printf("bg: %d: no such job\n", jid);
        return;
    }

    print_cmdline(theJob->pipe);
    printf("\n");

    int status = killpg(theJob->pids[0], SIGCONT);

    if (status == 0) {
        theJob->status = BACKGROUND;
    }
}

/*
 * Helper method to handle the stop command
 */
static void helper_stop(char* theJid) {
    if (theJid == NULL) {
        printf("stop: no such job\n");
        return;
    }

    int jid = atoi(theJid);
    struct job* theJob = get_job_from_jid(jid);

    if (theJob == NULL) {
        printf("stop: %d: no such job\n", jid);
        return;
    }

    int status = killpg(theJob->pids[0], SIGSTOP);

    if (status == 0) {
        if (theJob->status == FOREGROUND) {
            termstate_save(&theJob->saved_tty_state);
        }
        theJob->status = STOPPED;
    }
}
/*
 * Helper method to handle the kill method
 */
static void helper_kill(char* theJid) {
    if (theJid == NULL) {
        printf("kill: no such job\n");
        return;
    }

    int jid = atoi(theJid);
    struct job* theJob = get_job_from_jid(jid);

    if (theJob == NULL) {
        printf("kill: %d: no such job\n", jid);
        return;
    }

    int status = killpg(theJob->pids[0], SIGTERM);

    if (status == 0) {
        list_remove(&theJob->elem);
        delete_job(theJob);
    }
}

/*
 * Helper method to handle the jobs command
 */
static void helper_jobs() {
    int counter = 0;
    int i = 0;

    while (counter < list_size(&job_list) && i < MAXJOBS) {
        struct job* tempJob = jid2job[i];
        if (tempJob != NULL) {
            print_job(tempJob);
            counter++;
        }
        i++;
    }
}
/*
 * This method is called every time a command line is processed to interpret its
 * contents and call the right methods depending on the input of the user
 */
static void interpret(struct ast_command_line *cmdLine) {
    // Block SIGCHLD signals to prevent race conditions with job management
    signal_block(SIGCHLD);

    struct list *listPipe = &cmdLine->pipes; // Reference to the list of pipelines
    for (struct list_elem *e = list_begin(listPipe); e != list_end(listPipe);) {
        struct ast_pipeline *pipe = list_entry(e, struct ast_pipeline, elem);
        e = list_remove(e); // Removes the element to prevent double processing
        int resultSuccess = 0;
        struct list *listCommands = &pipe->commands; // Reference to the list of commands in the pipeline
        struct job *job = NULL;

        // Create pipes for inter-process communication
        int size = list_size(listCommands) - 1;
        if (size == 0) size++;
        int pipeArray[size][2]; // Array to hold pipe file descriptors
        int counter = 0; // Counter for command index

        // Iterate through each command in the pipeline
        for (struct list_elem *f = list_begin(listCommands); f != list_end(listCommands); f = list_next(f)) {
            struct ast_command *cmd = list_entry(f, struct ast_command, elem);
            char *cmdName = cmd->argv[0];

            // Handle built-in commands
            if (strcmp(cmdName, "exit") == 0) {
                exit(0);
            } else if (strcmp(cmdName, "cd") == 0) {
                helper_cd(cmd->argv[1]);
            } else if (strcmp(cmdName, "history") == 0) {
                helper_history();
            } else if (strcmp(cmdName, "jobs") == 0) {
                helper_jobs();
            } else if (strcmp(cmdName, "fg") == 0) {
                helper_fg(cmd->argv[1]);
            } else if (strcmp(cmdName, "bg") == 0) {
                helper_bg(cmd->argv[1]);
            } else if (strcmp(cmdName, "stop") == 0) {
                helper_stop(cmd->argv[1]);
            } else if (strcmp(cmdName, "kill") == 0) {
                helper_kill(cmd->argv[1]);
            } else {
                // Handle external commands with posix_spawn
                posix_spawn_file_actions_t child_file_attr;
                posix_spawnattr_t child_spawn_attr;

                posix_spawnattr_init(&child_spawn_attr);
                posix_spawn_file_actions_init(&child_file_attr);

                // Set attributes for the first command in the pipeline
                if (f == list_begin(listCommands)) {
                    job = add_job(pipe);
                    posix_spawnattr_setflags(&child_spawn_attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_TCSETPGROUP);
                    posix_spawnattr_setpgroup(&child_spawn_attr, 0);
                    posix_spawnattr_tcsetpgrp_np(&child_spawn_attr, termstate_get_tty_fd());

                    if (pipe->iored_input != NULL) {
                        posix_spawn_file_actions_addopen(&child_file_attr, 0, pipe->iored_input, O_RDONLY, 0777);
                    }
                } else {
                    posix_spawnattr_setflags(&child_spawn_attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_TCSETPGROUP);
                    posix_spawnattr_setpgroup(&child_spawn_attr, job->pids[0]);
                    posix_spawnattr_tcsetpgrp_np(&child_spawn_attr, termstate_get_tty_fd());
                }

                // Handle input and output redirection
                if (f == list_rbegin(listCommands)) {
                    if (pipe->append_to_output) {
                        posix_spawn_file_actions_addopen(&child_file_attr, 1, pipe->iored_output, O_WRONLY | O_APPEND | O_CREAT, 0777);
                    } else if (pipe->iored_output != NULL) {
                        posix_spawn_file_actions_addopen(&child_file_attr, 1, pipe->iored_output, O_WRONLY | O_TRUNC | O_CREAT, 0777);
                    }
                }

                // Set up piping between commands
                if (f == list_begin(listCommands) && f != list_rbegin(listCommands)) {
                    pipe2(pipeArray[counter], O_CLOEXEC);
                    posix_spawn_file_actions_adddup2(&child_file_attr, pipeArray[counter][PIPE_WRITE], STDOUT_FILENO);
                } else if (f != list_begin(listCommands) && f != list_rbegin(listCommands)) {
                    posix_spawn_file_actions_adddup2(&child_file_attr, pipeArray[counter][PIPE_WRITE], STDOUT_FILENO);
                    posix_spawn_file_actions_adddup2(&child_file_attr, pipeArray[counter - 1][PIPE_READ], STDIN_FILENO);
                } else if (f != list_begin(listCommands) && f == list_rbegin(listCommands)) {
                    posix_spawn_file_actions_adddup2(&child_file_attr, pipeArray[counter - 1][PIPE_READ], STDIN_FILENO);
                }

                // Redirect STDERR if specified
                if (cmd->dup_stderr_to_stdout) {
                    posix_spawn_file_actions_adddup2(&child_file_attr, STDOUT_FILENO, STDERR_FILENO);
                }

                // Spawn the child process
                pid_t childPID;
                extern char **environ;
                resultSuccess = posix_spawnp(&childPID, cmdName, &child_file_attr, &child_spawn_attr, cmd->argv, environ);

                if (resultSuccess == 0) {
                    // Find an empty slot for the child's PID and store it
                    int index = findEmptyPIDSlot(job);
                    job->pids[index] = childPID;
                    job->num_processes_alive++;

                    if (pipe->bg_job) {
                        job->status = BACKGROUND;
                        printf("[%u] %u\n", job->jid, childPID);
                        tcgetattr(termstate_get_tty_fd(), &job->saved_tty_state);
                    }

                }

                posix_spawn_file_actions_destroy(&child_file_attr);
                posix_spawnattr_destroy(&child_spawn_attr);
            }
            
            counter++; // Increment counter after processing each command
        }
        if (list_size(listCommands) > 1) {
            for (int i = 0; i < size; i++) {
                for (int j = 0; j < 2; j++) {
                    close(pipeArray[i][j]);
                }
            }
        }
        // If resultSuccess isn't zero, that means POSIX_SPAWN provided an error code
        if (resultSuccess != 0) {
            printf("no such file or directory\n");
            list_remove(&job->elem);
            delete_job(job);
        }

        // Waits for the job if it's in foreground status
        else if (job != NULL && job->status == FOREGROUND) {
            wait_for_job(job);

            // Safety check to make sure it wasn't stopped abruptly
            if (job->status == FOREGROUND) {
                list_remove(&job->elem);
                delete_job(job);
            }
        }

        // If job was null then frees the pipe
        else if (job == NULL) {
            ast_pipeline_free(pipe);
        }

        removeDoneJobs();
        termstate_give_terminal_back_to_shell();
    }
    signal_unblock(SIGCHLD);
}
int
main(int ac, char *av[]) {
    int opt;

    /* Process command-line arguments. See getopt(3) */
    while ((opt = getopt(ac, av, "h")) > 0) {
        switch (opt) {
            case 'h':
                usage(av[0]);
                break;
        }
    }

    list_init(&job_list);
    signal_set_handler(SIGCHLD, sigchld_handler);
    termstate_init();

    /* Read/eval loop. */
    for (;;) {
        /* If you fail this assertion, you were about to call readline()
         * without having terminal ownership.
         * This would lead to the suspension of your shell with SIGTTOU.
         * Make sure that you call termstate_give_terminal_back_to_shell()
         */
        assert(termstate_get_current_terminal_owner() == getpgrp());
        using_history();

        /* Do not output a prompt unless shell's stdin is a terminal */
        char *prompt = isatty(0) ? build_prompt() : NULL;
        char *cmdline = readline(prompt);
        free(prompt);

        char *output;
        int result = history_expand(cmdline, &output);

        if (result == 0) {
            add_history(cmdline);
        }

        if (result == 1) {
            add_history(output);
        }

        free(output);

        if (cmdline == NULL) /* User typed EOF */
            break;

        struct ast_command_line *cline = ast_parse_command_line(cmdline);
        free(cmdline);
        if (cline == NULL) /* Error in command line */
            continue;

        if (list_empty(&cline->pipes)) { /* User hit enter */
            ast_command_line_free(cline);
            continue;
        }

        // ast_command_line_print(cline);
        interpret(cline);
        /* Free the command line.
         * This will free the ast_pipeline objects still contained
         * in the ast_command_line. Once you implement a job list
         * that may take ownership of ast_pipeline objects that are
         * associated with jobs you will need to reconsider how you
         * manage the lifetime of the associated ast_pipelines.
         * Otherwise, freeing here will cause use-after-free errors.
         */
        ast_command_line_free(cline);
    }

    return 0;
}


