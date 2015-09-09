/*
 * tsh - A tiny shell program with job control
 *
 * Name: Minghan Chen    Andrew ID: minghan2
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF         0   /* undefined */
#define FG            1   /* running in foreground */
#define BG            2   /* running in background */
#define ST            3   /* stopped */

/* Define flags and modes of files*/
#define FLAG O_RDWR | O_CREAT | O_TRUNC
#define MODE S_IRUSR | S_IWUSR |S_IXUSR

/*
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Parsing states */
#define ST_NORMAL   0x0   /* next token is an argument */
#define ST_INFILE   0x1   /* next token is the input file */
#define ST_OUTFILE  0x2   /* next token is the output file */


/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t job_list[MAXJOBS]; /* The job list */

struct cmdline_tokens {
    int argc;               /* Number of arguments */
    char *argv[MAXARGS];    /* The arguments list */
    char *infile;           /* The input file */
    char *outfile;          /* The output file */
    enum builtins_t {       /* Indicates if argv[0] is a builtin command */
        BUILTIN_NONE,
        BUILTIN_QUIT,
        BUILTIN_JOBS,
        BUILTIN_BG,
        BUILTIN_FG} builtins;
};

/* End global variables */


/* Function prototypes */
void eval(char *cmdline);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, struct cmdline_tokens *tok);
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *job_list);
int maxjid(struct job_t *job_list);
int addjob(struct job_t *job_list, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *job_list, pid_t pid);
pid_t fgpid(struct job_t *job_list);
struct job_t *getjobpid(struct job_t *job_list, pid_t pid);
struct job_t *getjobjid(struct job_t *job_list, int jid);
int pid2jid(pid_t pid);
void listjobs(struct job_t *job_list, int output_fd);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

void fgsuspend(pid_t fg_pid);
int buildin_command(struct cmdline_tokens *token);
void execBuildin(struct cmdline_tokens *token);
void execFGBG(struct cmdline_tokens *token);

int getFD(char *file, int def);


/*
 * main - The shell's main routine
 */
int  main(int argc, char **argv)
{
    char c;
    char cmdline[MAXLINE];    /* cmdline for fgets */
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
            break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
            break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
            break;
        default:
            usage();
        }
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */
    Signal(SIGTTIN, SIG_IGN);
    Signal(SIGTTOU, SIG_IGN);

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler);

    /* Initialize the job list */
    initjobs(job_list);


    /* Execute the shell's read/eval loop */
    while (1) {
        //printf("in the while loop. \n");
        if (emit_prompt) {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin)) {
            /* End of file (ctrl-d) */
            printf ("\n");
            fflush(stdout);
            fflush(stderr);
            exit(0);
        }

        /* Remove the trailing newline */
        cmdline[strlen(cmdline)-1] = '\0';

        /* Evaluate the command line */
        eval(cmdline);
        //printf("after eval\n");

        fflush(stdout);
        fflush(stdout);
    }

    exit(0); /* control never reaches here */
}

/*
 * eval - Evaluate the command line that the user has just typed in
 *
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.
 */
void eval(char *cmdline)
{
    int bg;              /* should the job run in bg or fg? */
    struct cmdline_tokens tok;

    pid_t pid; //process id
    int state = UNDEF;

    sigset_t mask; // for blocking

    /* Parse command line */
    bg = parseline(cmdline, &tok);

    //printf("bg = %d \n", bg);

    if (bg == -1) {
        printf("parsing error\n");
        return;
    }/* parsing error */

    if (tok.argv[0] == NULL) {
        printf("empty line\n");
        return;
    }/* ignore empty lines */


    //printf("tok.argc = %d \n", tok.argc);
    //int i = 0;
    //for ( i = 0; i < tok.argc;i++) {
    //    printf("%s \n", tok.argv[i]);
    //}
    //printf("the judgement : %d. \n", buildin_command(&tok));

    if (buildin_command(&tok)) {
        //printf("hello.\n");
        execBuildin(&tok);
    } else { // not a buildin command
        //printf("here in buildin.\n");
        if (bg) {
            state = BG;
        } else {
            state = FG;
        }

        //block parent process for the SIGCHLD
        sigemptyset(&mask);
        sigaddset(&mask, SIGCHLD);
        sigaddset(&mask, SIGTSTP);
        sigaddset(&mask, SIGINT);
        sigprocmask(SIG_BLOCK, &mask, NULL); //Block Sigchld

        if ((pid = fork()) == 0) {  // for child process
            /* restore default handlers */
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
            sigprocmask(SIG_UNBLOCK, &mask, NULL); //Unblock sigchld

            if(setpgid(0, 0) < 0) { //else all the process are in the same group
				unix_error("setpgid error");
			}

            //change the table entry if the infile or outfile string in tok is not null
            dup2(getFD(tok.infile, STDIN_FILENO), STDIN_FILENO);
            dup2(getFD(tok.outfile, STDOUT_FILENO), STDOUT_FILENO);

            if (execve(tok.argv[0], tok.argv, environ) < 0) {
                printf("%s : Command not found in eval. \n", tok.argv[0]);
                exit(0);    //the child process terminate.
            }

        } else if (pid > 0){                    //for parent process

            if (addjob(job_list, pid, state, cmdline) == 0) {
                printf("failure when add job.\n");
                exit(0);
            } else {
                //printf("successfully add the job.\n");
                //printf("the corresponding jobID : %d and PID : %d. \n", pid2jid(pid), pid);
                sigprocmask(SIG_UNBLOCK, &mask, NULL);
                if (state == FG) {    //foreground
                    //printf("Here in state == FG. \n");
                    fgsuspend(pid);
                } else {    //background
                    printf("[%d] (%d) %s\n", pid2jid(pid), pid, cmdline);
                }
            }
        } else {
            unix_error("failed to fork new process");
        }
    }

    return;
}

/*This function is used to change the table entry of child processes when necessary*/
int getFD(char *file, int def) {
    int fd;
    if (file == NULL) {
        fd = def;
    } else {
        fd = open(file, FLAG, MODE);
    }

    if (fd < 0) {
        unix_error("wrong in getInFD./n");
        return -1;
    } else {
        return fd;
    }
}

/*This function is used when the user call a process running in frontground. It will wait
until signals invoke signal handlers*/
void fgsuspend(pid_t fg_pid) {
    while(fg_pid == fgpid(job_list)) {
		sleep(0);
	}
	//printf("over in fgsuspend. \n");
}

/*This function is used to check if the command is a buildin command*/
int buildin_command(struct cmdline_tokens *token) {
   if (token -> builtins == BUILTIN_NONE) {
        return 0;
   }

   return 1;
}

/*If the command is a buildin command, the shell will invoke this function to judge
which buildin command it is and execute corresponding operations*/
void execBuildin(struct cmdline_tokens *token) {
    if (token -> builtins == BUILTIN_QUIT) {
        //quit
        exit(0);
    }
    if (token -> builtins == BUILTIN_JOBS) {
        //Builtin_jobs

        int fd;
        if (token -> outfile != NULL) {
            fd = open(token -> outfile, FLAG, MODE);
        } else {
            fd = STDOUT_FILENO;
        }


        if (fd != -1) {
            listjobs(job_list, fd);
        } else {
            unix_error("wrong when list jobs.\n");
        }
    }
    if (token -> builtins == BUILTIN_BG || token -> builtins == BUILTIN_FG) {
        //FG or BG
        execFGBG(token);

    }

    return;
}
/*when the build in command is FG or BG*/
void  execFGBG(struct cmdline_tokens *token) {
    char * id;
    struct job_t *job;
    if (token -> argc < 1) {
        printf("You should provide jobID or processID. \n");
        return;
    }

    id = token -> argv[1];
    if (id[0] == '%') {    //jobID
        //printf("id in execFGBG is %s", id);
        id++;
        int jid = atoi(id);
        //printf("jobid in execFGBG is %d", jid);
        job = getjobjid(job_list, jid);
    } else {    //process ID
        int pid = atoi(id);
        job = getjobpid(job_list, pid);
    }

    if (job == NULL) {
        //printf("No corresponding job found! \n");
        return;
    } else if (job -> state != ST) {
        //printf("Corresponding job is not stoped. \n");
        return;
    } else {
        //printf("successfully get the job. \n");
        kill(-(job -> pid), SIGCONT);
    }

    if (token -> builtins == BUILTIN_BG) {    //Background
        job -> state = BG;
        printf("[%d] (%d) %s\n", job -> jid, job -> pid,
                job -> cmdline);
    } else {                                  //Frontground
        job -> state = FG;
        fgsuspend(job -> pid);
    }


}

/*
 * parseline - Parse the command line and build the argv array.
 *
 * Parameters:
 *   cmdline:  The command line, in the form:
 *
 *                command [arguments...] [< infile] [> oufile] [&]
 *
 *   tok:      Pointer to a cmdline_tokens structure. The elements of this
 *             structure will be populated with the parsed tokens. Characters
 *             enclosed in single or double quotes are treated as a single
 *             argument.
 * Returns:
 *   1:        if the user has requested a BG job
 *   0:        if the user has requested a FG job
 *  -1:        if cmdline is incorrectly formatted
 *
 * Note:       The string elements of tok (e.g., argv[], infile, outfile)
 *             are statically allocated inside parseline() and will be
 *             overwritten the next time this function is invoked.
 */

int parseline(const char *cmdline, struct cmdline_tokens *tok)
{

    static char array[MAXLINE];          /* holds local copy of command line */
    const char delims[10] = " \t\r\n";   /* argument delimiters (white-space) */
    char *buf = array;                   /* ptr that traverses command line */
    char *next;                          /* ptr to the end of the current arg */
    char *endbuf;                        /* ptr to end of cmdline string */
    int is_bg;                           /* background job? */

    int parsing_state;                   /* indicates if the next token is the
                                            input or output file */

    if (cmdline == NULL) {
        (void) fprintf(stderr, "Error: command line is NULL\n");
        return -1;
    }

    (void) strncpy(buf, cmdline, MAXLINE);
    endbuf = buf + strlen(buf);

    tok->infile = NULL;
    tok->outfile = NULL;

    /* Build the argv list */
    parsing_state = ST_NORMAL;
    tok->argc = 0;

    while (buf < endbuf) {
        /* Skip the white-spaces */
        buf += strspn (buf, delims);
        if (buf >= endbuf) break;

        /* Check for I/O redirection specifiers */
        if (*buf == '<') {
            if (tok->infile) {
                (void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
                return -1;
            }
            parsing_state |= ST_INFILE;
            buf++;
            continue;
        }
        if (*buf == '>') {
            if (tok->outfile) {
                (void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
                return -1;
            }
            parsing_state |= ST_OUTFILE;
            buf ++;
            continue;
        }

        if (*buf == '\'' || *buf == '\"') {
            /* Detect quoted tokens */
            buf++;
            next = strchr (buf, *(buf-1));
        } else {
            /* Find next delimiter */
            next = buf + strcspn (buf, delims);
        }

        if (next == NULL) {
            /* Returned by strchr(); this means that the closing
               quote was not found. */
            (void) fprintf (stderr, "Error: unmatched %c.\n", *(buf-1));
            return -1;
        }

        /* Terminate the token */
        *next = '\0';

        /* Record the token as either the next argument or the i/o file */
        switch (parsing_state) {
        case ST_NORMAL:
            tok->argv[tok->argc++] = buf;
            break;
        case ST_INFILE:
            tok->infile = buf;
            break;
        case ST_OUTFILE:
            tok->outfile = buf;
            break;
        default:
            (void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
            return -1;
        }
        parsing_state = ST_NORMAL;

        /* Check if argv is full */
        if (tok->argc >= MAXARGS-1) break;

        buf = next + 1;
    }

    if (parsing_state != ST_NORMAL) {
        (void) fprintf(stderr,
                       "Error: must provide file name for redirection\n");
        return -1;
    }

    /* The argument list must end with a NULL pointer */
    tok->argv[tok->argc] = NULL;

    if (tok->argc == 0)  /* ignore blank line */
        return 1;

    if (!strcmp(tok->argv[0], "quit")) {                 /* quit command */
        tok->builtins = BUILTIN_QUIT;
    } else if (!strcmp(tok->argv[0], "jobs")) {          /* jobs command */
        tok->builtins = BUILTIN_JOBS;
    } else if (!strcmp(tok->argv[0], "bg")) {            /* bg command */
        tok->builtins = BUILTIN_BG;
    } else if (!strcmp(tok->argv[0], "fg")) {            /* fg command */
        tok->builtins = BUILTIN_FG;
    } else {
        tok->builtins = BUILTIN_NONE;
    }

    /* Should the job run in the background? */
    if ((is_bg = (*tok->argv[tok->argc-1] == '&')) != 0)
        tok->argv[--tok->argc] = NULL;

    return is_bg;
}


/*****************
 * Signal handlers
 *****************/

/*
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP, SIGTSTP, SIGTTIN or SIGTTOU signal. The
 *     handler reaps all available zombie children, but doesn't wait
 *     for any other currently running children to terminate.
 */
void sigchld_handler(int sig)
{
    pid_t childpid;
    int status; // If the process was stopped or terminated by SIGINT OR ...
    while ((childpid = waitpid(-1, &status, WNOHANG | WUNTRACED))) {
        if (childpid == 0) {
            printf("no zombie child \n");
            break;
        } else if (childpid > 0) {
            if (WIFSTOPPED(status)) {
                //printf("stopped by SIGTSTP \n");
                struct job_t *job = getjobpid(job_list, childpid);
                job -> state = ST;
                printf("Job [%d] (%d) stopped by signal %d\n",
                    job -> jid, childpid, WSTOPSIG(status));
            } else if (WIFSIGNALED(status)) {
                //printf("terminated by SIGINT. \n");
                printf("Job [%d] (%d) terminated by signal %d\n",
                    pid2jid(childpid), childpid, WTERMSIG(status));
                deletejob(job_list, childpid);
            } else if (WIFEXITED(status)) {
                //printf("terminate normally. \n");
                deletejob(job_list, childpid);
                //printf("delete: %d.\n",i);
            } else {
                printf("others \n");
            }
        } else {
            if (errno == ECHILD) {
                //printf("no child processes now. \n");
                break;
            } else {
                unix_error("waitpid error. \n");
            }

        }
    }
    return;
}

/*
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.
 */
void sigint_handler(int sig)
{   pid_t fg_pid = fgpid(job_list);
    if (fg_pid) {
        kill(-fg_pid, sig);
    }
    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.
 */
void sigtstp_handler(int sig)
{
    pid_t fg_pid = fgpid(job_list);

    if (fg_pid) {
        kill(-fg_pid, sig);
    }

    return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void
clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void
initjobs(struct job_t *job_list) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        clearjob(&job_list[i]);
}

/* maxjid - Returns largest allocated job ID */
int
maxjid(struct job_t *job_list)
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].jid > max)
            max = job_list[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int
addjob(struct job_t *job_list, pid_t pid, int state, char *cmdline)
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (job_list[i].pid == 0) {
            job_list[i].pid = pid;
            job_list[i].state = state;
            job_list[i].jid = nextjid++;
            if (nextjid > MAXJOBS)
                nextjid = 1;
            strcpy(job_list[i].cmdline, cmdline);
            if(verbose){
                printf("Added job [%d] %d %s\n",
                       job_list[i].jid,
                       job_list[i].pid,
                       job_list[i].cmdline);
            }
            return 1;
        }
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int
deletejob(struct job_t *job_list, pid_t pid)
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (job_list[i].pid == pid) {
            clearjob(&job_list[i]);
            nextjid = maxjid(job_list)+1;
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t
fgpid(struct job_t *job_list) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].state == FG)
            return job_list[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t
*getjobpid(struct job_t *job_list, pid_t pid) {
    int i;

    if (pid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].pid == pid)
            return &job_list[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *job_list, int jid)
{
    int i;

    if (jid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].jid == jid)
            return &job_list[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int
pid2jid(pid_t pid)
{
    int i;

    if (pid < 1)
        return 0;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].pid == pid) {
            return job_list[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void
listjobs(struct job_t *job_list, int output_fd)
{
    int i;
    char buf[MAXLINE];

    for (i = 0; i < MAXJOBS; i++) {
        memset(buf, '\0', MAXLINE);
        if (job_list[i].pid != 0) {
            sprintf(buf, "[%d] (%d) ", job_list[i].jid, job_list[i].pid);
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
            memset(buf, '\0', MAXLINE);
            switch (job_list[i].state) {
            case BG:
                sprintf(buf, "Running    ");
                break;
            case FG:
                sprintf(buf, "Foreground ");
                break;
            case ST:
                sprintf(buf, "Stopped    ");
                break;
            default:
                sprintf(buf, "listjobs: Internal error: job[%d].state=%d ",
                        i, job_list[i].state);
            }
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
            memset(buf, '\0', MAXLINE);
            sprintf(buf, "%s\n", job_list[i].cmdline);
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
        }
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void
usage(void)
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void
unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void
app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t
*Signal(int signum, handler_t *handler)
{
    struct sigaction action, old_action;

    action.sa_handler = handler;
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
        unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig)
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}


