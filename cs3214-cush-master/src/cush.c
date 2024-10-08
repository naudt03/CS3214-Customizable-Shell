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
#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <termios.h>
#include <sys/wait.h>
#include <assert.h>
#include <fcntl.h>
#include <readline/history.h>
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
    FINISHED,         /* job is finished running */
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
    case FINISHED:
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
/* Print a job */
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
 * in handle_child_status because wait_for_job assumes that
 * even jobs with no more num_processes_alive haven't been
 * deallocated.  You should postpone deleting completed
 * jobs from the job list until when your code will no
 * longer touch them.
 *
 * The code below relies on `job->status` having been set to FOREGROUND
 * and `job->num_processes_alive` having been set to the number of
 * processes successfully forked for this job.
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
    // Step 1 determine job with job ID
    struct job* theJob = NULL;
    for (struct list_elem* e = list_begin(&job_list); e != list_end(&job_list); e = list_next(e)) {
        struct job* aJob = list_entry(e, struct job, elem);
        for (int j = 0; j < MAXJOBS; j++) {
            if (aJob->pids[j] == pid) {
                theJob = aJob;
                break;
            }
        }
    }
    if (theJob == NULL) {
        printf("HANDLE CHILD STATUS: JOB NOT FOUND\n");
        return;
    }
    // Step 2 and 3 determine status change and adjust number of processes
    if (WIFEXITED(status)) {
        theJob->num_processes_alive--;
        if (theJob->status == FOREGROUND && status == 0) {
            termstate_sample();
        }
        else if (theJob->status == BACKGROUND) {
        theJob->status = FINISHED;
        print_job(theJob);
        }
    } 
    else if (WIFSIGNALED(status)) {
        theJob->num_processes_alive--;
        if (theJob->status == BACKGROUND) {
            theJob->status = FINISHED;
        }
        int signal = WTERMSIG(status);
        printf(strsignal(signal)); // Prints what terminated the process
    } 
    else if (WIFSTOPPED(status)) {
        if (theJob->status == FOREGROUND) {
            termstate_save(&theJob->saved_tty_state);
        }
        theJob->status = STOPPED;
        if (WSTOPSIG(status) == SIGTSTP) {
            print_job(theJob);
        }
    }
}

// Function to implement 'ls' command
static void cush_ls(void) {
    struct dirent *entry;
    DIR *dp = opendir(".");

    if (dp == NULL) {
        perror("opendir");
        return;
    }

    // Read and print all the files and directories in the current directory
    while ((entry = readdir(dp))) {
        // Skip the "." and ".." directories
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            printf("%s\t", entry->d_name);
        }
    }
    printf("\n");
    closedir(dp);
}
static void cush_history() {
    HIST_ENTRY** historyList = history_list();
    int j = 0;
    if (historyList == NULL) {
        printf("No history found\n");
        return;
    }
    while (j < history_length) {
        printf("%d: %s\n", j + history_base, historyList[j]->line);
        j++;
    }
}
static void cush_pwd() {
    char cwd[PATH_MAX];  // PATH_MAX is a constant that defines the maximum length of a file path
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("%s\n", cwd);  // Print the current working directory
    } else {
        perror("getcwd() error");  // Print an error if getcwd fails
    }
}
/*
 * Function that implements the bg command
 */
static void cush_bg(char* inpJid) {
    if (inpJid == NULL) {
        printf("bg: current: no such job\n");
        return;
    }
    int jid = atoi(inpJid);
    struct job* inpJob = get_job_from_jid(jid);
    if (inpJob == NULL) {
        printf("bg: %d: no such job\n", jid);
        return;
    }
    print_cmdline(inpJob->pipe);
    printf("\n");
    int status = killpg(inpJob->pids[0], SIGCONT);
    if (status == 0) {
        inpJob->status = BACKGROUND;
    }
}
/*
 * Function that implements the fg command
 */
static void cush_fg(char* inpJid) {
    if (inpJid == NULL) {
        printf("fg: current: no such job\n");
        return;
    }
    int jid = atoi(inpJid);
    struct job* inpJob = get_job_from_jid(jid);
    if (inpJob == NULL) {
        printf("fg: %d: no such job\n", jid);
        return;
    }
    // call tcsetattr and tcsetpgrp
    tcsetattr(termstate_get_tty_fd(), TCSANOW, &inpJob->saved_tty_state);
    tcsetpgrp(termstate_get_tty_fd(), inpJob->pids[0]);
    print_cmdline(inpJob->pipe);
    printf("\n");
    int status = killpg(inpJob->pids[0], SIGCONT);
    if (status == 0) {
        inpJob->status = FOREGROUND;
        termstate_give_terminal_to(&inpJob->saved_tty_state, inpJob->pids[0]);
        wait_for_job(inpJob);
        if (inpJob->status == FOREGROUND) {
            if (status == 0) {
                if (inpJob->status == FOREGROUND) {
                    list_remove(&inpJob->elem);
                    delete_job(inpJob);
                }
            }
        }
    }
}

/*
 * Function that implements the kill method
 */
static void cush_kill(char* inpJid) {
    if (inpJid == NULL) {
        printf("kill: no such job\n");
        return;
    }
    int jid = atoi(inpJid);
    struct job* inpJob = get_job_from_jid(jid);
    if (inpJob == NULL) {
        printf("kill: %d: no such job\n", jid);
        return;
    }
    int status = killpg(inpJob->pids[0], SIGTERM);
    if (status == 0) {
        list_remove(&inpJob->elem);
        delete_job(inpJob);
    }
}
/*
 * Function that implements stop command
 */
static void cush_stop(char* inpJid) {
    if (inpJid == NULL) {
        printf("stop: no such job\n");
        return;
    }
    int jid = atoi(inpJid);
    struct job* inpJob = get_job_from_jid(jid);
    if (inpJob == NULL) {
        printf("stop: %d: no such job\n", jid);
        return;
    }
    int status = killpg(inpJob->pids[0], SIGSTOP);
    if (status == 0) {
        if (inpJob->status == FOREGROUND) {
            termstate_save(&inpJob->saved_tty_state);
        }
        inpJob->status = STOPPED;
    }
}
/*
 * Removes finished jobs
 */
static void removeFinishedJobs() {
    int cnt = 0;
    int j = 0;
    while (cnt < list_size(&job_list) && j < MAXJOBS) {
        struct job* aJob = jid2job[j];
        if (aJob!= NULL) {
            if (aJob ->status == FINISHED) {
                list_remove(&aJob ->elem);
                delete_job(aJob);
            }
            cnt++;
        }
        j++;
    }
}
/*
 * Function that implements the jobs command
 */
static void cush_jobs() {
    int cnt = 0;
    int i = 0;
    while (cnt < list_size(&job_list) && i < MAXJOBS) {
        struct job* aJob = jid2job[i];
        if (aJob != NULL) {
            print_job(aJob);
            cnt++;
        }
        i++;
    }
}
/*
* This function interprets the command line entered and calls the cush  * functions corresponding to it
 */
static void interpret(struct ast_command_line *inpCmdLine) {
    signal_block(SIGCHLD); //signal to prevent race condition
    struct list *listPipe = &inpCmdLine->pipes;
    for (struct list_elem *e = list_begin(listPipe); e != list_end(listPipe);) {
        struct ast_pipeline *pipe = list_entry(e, struct ast_pipeline, elem);
        e = list_remove(e); // Remove to stop double processing
        int returnCode = 0;
        struct list *listCommands = &pipe->commands; 
        struct job *job = NULL;
        // Create pipes for processes
        int size = list_size(listCommands) - 1;
        if (size == 0) size++;
        int pipeArray[size][2]; // Array for file descriptors
        int cnt = 0; // Counter for command index
        for (struct list_elem *f = list_begin(listCommands); f != list_end(listCommands); f = list_next(f)) {
            struct ast_command *cmd = list_entry(f, struct ast_command, elem);
            char *inpCmd = cmd->argv[0];
            if (strcmp(inpCmd, "exit") == 0) {
                exit(0);
            } else if (strcmp(inpCmd, "bg") == 0) {
                cush_bg(cmd->argv[1]);
            } else if (strcmp(inpCmd, "ls") == 0) {
                cush_ls();
            } else if (strcmp(inpCmd, "pwd") == 0) {
                cush_pwd();
            } else if (strcmp(inpCmd, "history") == 0) {
                cush_history();
            } else if (strcmp(inpCmd, "fg") == 0) {
                cush_fg(cmd->argv[1]);
            } else if (strcmp(inpCmd, "kill") == 0) {
                cush_kill(cmd->argv[1]);
            } else if (strcmp(inpCmd, "stop") == 0) {
                cush_stop(cmd->argv[1]);
            } else if (strcmp(inpCmd, "jobs") == 0) {
                cush_jobs();
            } else {
                posix_spawn_file_actions_t spawn_child_file;
                posix_spawnattr_t spawn_child_attr;
                posix_spawnattr_init(&spawn_child_attr);
                posix_spawn_file_actions_init(&spawn_child_file);
                if (f == list_begin(listCommands)) {
                    job = add_job(pipe);
                    posix_spawnattr_setflags(&spawn_child_attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_TCSETPGROUP);
                    posix_spawnattr_setpgroup(&spawn_child_attr, 0);
                    posix_spawnattr_tcsetpgrp_np(&spawn_child_attr, termstate_get_tty_fd());
                    if (pipe->iored_input != NULL) {
                        posix_spawn_file_actions_addopen(&spawn_child_file, 0, pipe->iored_input, O_RDONLY, 0777);
                    }
                } else {
                    posix_spawnattr_setflags(&spawn_child_attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_TCSETPGROUP);
                    posix_spawnattr_setpgroup(&spawn_child_attr, job->pids[0]);
                    posix_spawnattr_tcsetpgrp_np(&spawn_child_attr, termstate_get_tty_fd());
                }
                if (f == list_rbegin(listCommands)) {
                    if (pipe->append_to_output) {
                        posix_spawn_file_actions_addopen(&spawn_child_file, 1, pipe->iored_output, O_WRONLY | O_APPEND | O_CREAT, 0777);
                    } else if (pipe->iored_output != NULL) {
                        posix_spawn_file_actions_addopen(&spawn_child_file, 1, pipe->iored_output, O_WRONLY | O_TRUNC | O_CREAT, 0777);
                    }
                }
                // Pipe the commands
                if (f == list_begin(listCommands) && f != list_rbegin(listCommands)) {
                    pipe2(pipeArray[cnt], O_CLOEXEC);
                    posix_spawn_file_actions_adddup2(&spawn_child_file, pipeArray[cnt][PIPE_WRITE], STDOUT_FILENO);
                } else if (f != list_begin(listCommands) && f != list_rbegin(listCommands)) {
                    posix_spawn_file_actions_adddup2(&spawn_child_file, pipeArray[cnt][PIPE_WRITE], STDOUT_FILENO);
                    posix_spawn_file_actions_adddup2(&spawn_child_file, pipeArray[cnt - 1][PIPE_READ], STDIN_FILENO);
                } else if (f != list_begin(listCommands) && f == list_rbegin(listCommands)) {
                    posix_spawn_file_actions_adddup2(&spawn_child_file, pipeArray[cnt - 1][PIPE_READ], STDIN_FILENO);
                }
                if (cmd->dup_stderr_to_stdout) {
                    posix_spawn_file_actions_adddup2(&spawn_child_file, STDOUT_FILENO, STDERR_FILENO);
                }
                // Spawn the child process
                pid_t childPID;
                extern char **environ;
                returnCode = posix_spawnp(&childPID, inpCmd, &spawn_child_file, &spawn_child_attr, cmd->argv, environ);
                if (returnCode == 0) {
                    // Add child
                    int index = findEmptyPIDSlot(job);
                    job->pids[index] = childPID;
                    job->num_processes_alive++;
                    if (pipe->bg_job) {
                        job->status = BACKGROUND;
                        printf("[%u] %u\n", job->jid, childPID);
                        tcgetattr(termstate_get_tty_fd(), &job->saved_tty_state);
                    }
                }
                posix_spawn_file_actions_destroy(&spawn_child_file);
                posix_spawnattr_destroy(&spawn_child_attr);
            }
            
            cnt++;
        }
        if (list_size(listCommands) > 1) {
            for (int i = 0; i < size; i++) {
                for (int j = 0; j < 2; j++) {
                    close(pipeArray[i][j]);
                }
            }
        }
        // If returnCode isn't zero, that means POSIX_SPAWN provided an error code
        if (returnCode != 0) {
            printf("no such file or directory\n");
            list_remove(&job->elem);
            delete_job(job);
        }
        // Wait for foreground job status
        else if (job != NULL && job->status == FOREGROUND) {
            wait_for_job(job);
            if (job->status == FOREGROUND) {
                list_remove(&job->elem);
                delete_job(job);
            }
        }
        else if (job == NULL) {
            ast_pipeline_free(pipe);
        }
        removeFinishedJobs();
        termstate_give_terminal_back_to_shell();
    }
    signal_unblock(SIGCHLD);
}
/*
 * Main function that runs the cush shell
 */

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
    using_history(); //initialize history
    /* Read/eval loop. */
    for (;;) {
        /* If you fail this assertion, you were about to call readline()
         * without having terminal ownership.
         * This would lead to the suspension of your shell with SIGTTOU.
         * Make sure that you call termstate_give_terminal_back_to_shell()
         */
        assert(termstate_get_current_terminal_owner() == getpgrp());
        /* Do not output a prompt unless shell's stdin is a terminal */
        char *prompt = isatty(0) ? build_prompt() : NULL;
        char *cmdline = readline(prompt);
        free(prompt);
        if (cmdline == NULL) /* User typed EOF */
            break;
        struct ast_command_line *cline = ast_parse_command_line(cmdline);
        add_history(cmdline);
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

