/* tsh - A tiny shell program with job control
 * CISC 361 - Lab 1 (Simple Shell)
 * Benjamin Steenkamer
 * Abraham McIlvaine
 * 4/13/17
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/*
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

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
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv);
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs);
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid);
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid);
int pid2jid(pid_t pid);
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine
 */
int main(int argc, char **argv){
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    if(dup2(1, 2) < 0){
        unix_error("dup2 error");
    }

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

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler);

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1){
    	/* Read command line */
    	if (emit_prompt){
    	    printf("%s", prompt);
    	    fflush(stdout);
    	}
    	if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
    	    app_error("fgets error");
    	if (feof(stdin)) { /* End of file (ctrl-d) */
    	    fflush(stdout);
    	    exit(0);
    	}

    	/* Evaluate the command line */
        fflush(stdout);
    	eval(cmdline);
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
void eval(char *cmdline){
    char *argv[MAXARGS]; //Contains the command line command and arguments.
    int bg; //True if the job will run in the bg.
    pid_t pid; //Process ID of the job.

    //Parse the cmdline and put it into argv format.
    //Also set whether the process is to run in the bg.
    bg = parseline(cmdline, argv);

    //Ignore any blank inputs.
    if(argv[0] == NULL){
        return;
    }

    //See if command is built in. If it is, run it right away.
    //Otherwise, create a job to handle it.
    if(!builtin_cmd(argv)){

        //Parent blocks SIGCHLD signals before fork to avoid race condition.
        sigset_t mask;

        //Initialize the signal set pointed to by mask.
        if(sigemptyset(&mask) < 0){
            //Returning a negative value means it was not able to initialize the signal set.
            unix_error("sigemptyset error");
        }

        //Add SIGCHLD to the mask signal set pointed to by mask.
        if(sigaddset(&mask, SIGCHLD) < 0){
            //Returning a negative value means it was not able to add SIGCHLD to the signal set.
            unix_error("sigaddset error");
        }

        //Change the signal mask to have SIG_BLOCK.
        if(sigprocmask(SIG_BLOCK, &mask, NULL) < 0){
            //Returning a negative value means it was not able change the signal mask.
            unix_error("sigprocmask error (SIG_BLOCK)");
        }

        //Create a child process to run the new job.
        if((pid = fork()) < 0){
            //If fork returns a negative value, it failed to create a child process.
            unix_error("fork error");
        }

        //The child now runs the new job.
        if(pid == 0){

            //Give child a new process group ID so bg children don't receive SIGINT or SIGTSTP from ctrl+c.
            if(setpgid(0,0) < 0){
                unix_error("setpgid error"); //Unable to set group ID of child process.
            }

            //Unblock SIGCHLD signals since child inherited blocked vectors from parent.
            if(sigprocmask(SIG_UNBLOCK,&mask,NULL) < 0){
                //Returning a negative value means it was not able change the signal mask to have SIG_UNBLOCK.
                unix_error("sigprocmask error (SIG_UNBLOCK)");
            }

            //Run the program.
            if(execve(argv[0], argv, environ) < 0){
                //If execve() returns a negative value, the program could not be found.
                printf("%s: Command not found.\n", argv[0]);
                exit(0);
            }
        }

        //The parent must now either wait on the fg job or print out details on the bg job.
        if(!bg){ //The created job is running in the fg.
            addjob(jobs, pid, FG, cmdline); //Add the fg job to the job list.

            //Unblock SIGCHLD signals.
            if(sigprocmask(SIG_UNBLOCK, &mask, NULL) < 0){
                //Returning a negative value means it was not able change the signal mask to have SIG_UNBLOCK.
                unix_error("sigprocmask error (SIG_UNBLOCK)");
            }

            waitfg(pid); //Wait on the fg job to finish before proceeding.
        }
        else{ //The created job is running in the bg.
            addjob(jobs, pid, BG, cmdline); //Add the bg job to the job list.

            //Unblock SIGCHLD signals.
            if(sigprocmask(SIG_UNBLOCK,&mask,NULL) < 0){
                //Returning a negative value means it was not able change the signal mask to have SIG_UNBLOCK.
                unix_error("sigprocmask error (SIG_UNBLOCK)");
            }

            //Print out details on the bg job.
            printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline);
        }
    }

    return;
}

/*
 * parseline - Parse the command line and build the argv array.
 *
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.
 */
int parseline(const char *cmdline, char **argv){

    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
	buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
	buf++;
	delim = strchr(buf, '\'');
    }
    else {
	delim = strchr(buf, ' ');
    }

    while (delim) {
	argv[argc++] = buf;
	*delim = '\0';
	buf = delim + 1;
	while (*buf && (*buf == ' ')) /* ignore spaces */
	       buf++;

	if (*buf == '\'') {
	    buf++;
	    delim = strchr(buf, '\'');
	}
	else {
	    delim = strchr(buf, ' ');
	}
    }
    argv[argc] = NULL;

    if (argc == 0)  /* ignore blank line */
	return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0) {
	argv[--argc] = NULL;
    }
    return bg;
}

/*
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately. Return 0 if it is not a built-in command.
 *    Return 1 if it is a built-in command.
 */
int builtin_cmd(char **argv){
    if(strcmp(argv[0], "quit") == 0){
        //Exit the shell.
        exit(0);
    }
    else if(strcmp(argv[0], "jobs") == 0){
        //Print the current jobs list.
        listjobs(jobs);

        return 1;
    }
    else if(strcmp(argv[0], "bg") == 0){
        //Execute the builtin bg and fg commands
        do_bgfg(argv);

        return 1;
    }
    else if(strcmp(argv[0], "fg") == 0){
        //Execute the builtin bg and fg commands
        do_bgfg(argv);

        return 1;
    }

    return 0;     /* not a builtin command */
}

/*
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv){
    struct job_t *job;

    //If bg of fg command is entered with no argument, it is invalid.
    if(argv[1] == NULL){
        printf("%s command requires PID or %%jobid argument\n", argv[0]);
        return;
    }

    //Arguements for bg can be a "%JID" or "PID".
    //If arguement is a %JID.
    if((argv[1][0] == '%')){
        if(atoi(argv[1] + 1) == 0){ //If bg or fg command is entered with the argument %NaN, it is invalid.
            printf("%s: argument must be a PID or %%jobid\n", argv[0]);
            return;
        }

        //Convert the JID to an integer (skipping the % symbol) and find the corresponding job.
        job = getjobjid(jobs, atoi(argv[1] + 1));

        //If no job was found.
        if(job == NULL){
            printf("%s: No such job\n", argv[1]);
            return;
        }
    }
    else{// If arguement is a PID.
        if(atoi(argv[1]) == 0){ //If bg of fg command is entered with the argument NaN, it is invalid.
            printf("%s: argument must be a PID or %%jobid\n", argv[0]);
            return;
        }

        //Convert the arguemnt (a PID) to an interger and find the corresponding job.
        job = getjobpid(jobs, atoi(argv[1])); //argv[1] contains the PID

        //If no job was found.
        if(job == NULL){
            printf("(%d): No such process\n", atoi(argv[1]));
            return;
        }
    }

    //Send the start signal to the stopped job.
    //Use -PID so SIGCONT is sent to all processes with process group ID(PGID) equal to |-PID|.
    if(kill(-(job->pid), SIGCONT) < 0){
        //If kill returns a negative value, it was not able to send the signal.
        unix_error("kill error (do_bgfg)");
    }

    //Update the job's status.
    if(strcmp(argv[0], "bg") == 0){//Set the job's status to running in the bg.
        job->state= BG;

        //Now that the job is running in the bg, print out the bg job details.
        printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline);
    }
    else{//Set the job's status to running in the fg.
        job->state= FG;

        //Now that the job is running in the fg, must wait until it is finshed.
        waitfg(job->pid);
    }

    return;
}

/*
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid){
    //Don't want to use waitpid here; creates a mess.
    // int status;
    // if(waitpid(pid, &status, 0) < 0){ //Parent waits for child process (a fg job) to end.
    //     unix_error("waitfg: waitpid error"); //Catch error
    // }

    //While fg job is still active.
    while (fgpid(jobs) != 0){
        //Continue to sleep until fg is no longer active.
        sleep(1);
    }

    return;
}

/*****************
 * Signal handlers
 *****************/

/*
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.
 */
void sigchld_handler(int sig){
    int status = 0;
    pid_t pid;

    //Reap all available zombie children or handle stopped children.
    //If none of the children have terminated OR none of the children are stopped (pid = 0), exit loop.
    while ((pid = waitpid(-1, &status, WNOHANG|WUNTRACED)) != 0){
        if(pid < 0){
            //If waitpid returns a negative value, it has no child processes at all.
            return;
        }

        //Remove terminated job or edit status of stopped job.
        if(pid > 0){

            //If the child process did not terminate normally through an exit or return, give reason.
            if(!WIFEXITED(status)){
                //If the process was terminated by a signal that was not caught, report the signal.
                if(WIFSIGNALED(status)){
                    printf("Job [%d] (%d) terminated by signal %d\n", pid2jid(pid), pid, WTERMSIG(status));
                }
            }

            //If the child is stopped, don't remove it from the job list, but change its state to stopped (ST).
            if(WIFSTOPPED(status)){
                //Find the stopped job with its PID and change its state.
                getjobpid(jobs, pid)->state = ST;

                //Report that the job was stopped and by what sign.
                printf("Job [%d] (%d) stopped by signal %d\n", pid2jid(pid), pid, WSTOPSIG(status));
            }
            else{
                //When a child has been reaped, delete its job from the job list.
                deletejob(jobs, pid);
            }
        }
    }

    return;
}

/*
 * sigint_handler - The kernel sends a SIGINT to the shell whenever the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.
 */
void sigint_handler(int sig){
    //Get the fg job pid, if there is a fg job.
    pid_t pid = fgpid(jobs);

    //If pid = 0, then there is no running fg to terminate.
    if(pid != 0){
        //Send SIGINT signal to every process in the fg group.
        if(kill(-pid, sig) < 0){
            //If kill returns a negative value, an error occurred.
            unix_error("kill error (sigint_handler)");
        }
    }

    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.
 */
void sigtstp_handler(int sig){
    //Get the fg job pid, if there is a fg job.
    pid_t pid = fgpid(jobs);

    //If pid = 0, then there is no running fg to stop.
    if(pid != 0){
        //Send SIGTSTP signal to every process in the fg group.
        if(kill(-pid, sig) < 0){
            //If kill returns a negative value, an error occurred.
            unix_error("kill error (sigtstp_handler)");
        }
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
void clearjob(struct job_t *job){
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs){
    int i;

    for (i = 0; i < MAXJOBS; i++)
	   clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs){
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
    	if (jobs[i].jid > max)
    	    max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline){
    int i;

    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
    	if (jobs[i].pid == 0) {
    	    jobs[i].pid = pid;
    	    jobs[i].state = state;
    	    jobs[i].jid = nextjid++;
    	    if (nextjid > MAXJOBS)
    		nextjid = 1;
    	    strcpy(jobs[i].cmdline, cmdline);
      	    if(verbose){
    	        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
                }
                return 1;
    	}
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid){
    int i;

    if (pid < 1)
	   return 0;

    for (i = 0; i < MAXJOBS; i++) {
    	if (jobs[i].pid == pid) {
    	    clearjob(&jobs[i]);
    	    nextjid = maxjid(jobs)+1;
    	    return 1;
    	}
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs){
    int i;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].state == FG)
	    return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid){
    int i;

    if (pid < 1)
	   return NULL;

    for (i = 0; i < MAXJOBS; i++)
    	if (jobs[i].pid == pid)
    	    return &jobs[i];

    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid){
    int i;

    if (jid < 1)
	   return NULL;

    for (i = 0; i < MAXJOBS; i++)
    	if (jobs[i].jid == jid)
    	    return &jobs[i];

    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid){
    int i;

    if (pid < 1)
	   return 0;
    for (i = 0; i < MAXJOBS; i++)
    	if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }

    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs){
    int i;

    for (i = 0; i < MAXJOBS; i++){
    	if (jobs[i].pid != 0) {
    	    printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
    	    switch (jobs[i].state) {
    		case BG:
    		    printf("Running ");
    		    break;
    		case FG:
    		    printf("Foreground ");
    		    break;
    		case ST:
    		    printf("Stopped ");
    		    break;
    	    default:
    		    printf("listjobs: Internal error: job[%d].state=%d ", i, jobs[i].state);
    	    }
    	    printf("%s", jobs[i].cmdline);
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
void usage(void){
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg){
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg){
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler){
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
void sigquit_handler(int sig){
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}
