#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <assert.h>

#include <sys/wait.h>
#include <sys/types.h>

#include "proc-common.h"
#include "request.h"

/*vm Compile-time parameters. */
#define SCHED_TQ_SEC 3                /* time quantum */
#define TASK_NAME_SZ 60               /* maximum size for a task's name */
#define SHELL_EXECUTABLE_NAME "shell" /* executable for shell */

int tasknum;

struct node{
    int id;
    int pid;
    char name[TASK_NAME_SZ];
    struct node *next;
} *first, *last;

int empty(void){
    if (first == NULL) return 1;
    return 0;
};

void enqueue(int newID, int newPID, char newName[TASK_NAME_SZ]){
    struct node *temp = (struct node *) malloc(sizeof(struct node));
    temp->id = newID;
    temp->pid = newPID;
    strcpy(temp->name, newName);
    temp->next = NULL;
    if (empty() == 0){
        last->next = temp;
        last = temp;
    }
    else{
        first = temp;
        last = temp;
    };
    
};

void dequeue(void){
    if (empty() == 1) return;
    if (first != last){
        struct node *temp = first;
        first = first->next;
        free(temp);

    }
    else if((first->id)!=0)
    {
        free(first);
        first = NULL;
        last = NULL;
    };
};


/* Print a list of all tasks currently being scheduled.  */
static void
sched_print_tasks(void)
{
	struct node *tempo = first;
	printf("TASK QUEUE\n");
	while (tempo != NULL){
		printf("id=%d , pid=%d , name=%s \n", tempo->id,tempo->pid,tempo->name);
		tempo = tempo->next;
	};
	
	
}

/* Send SIGKILL to a task determined by the value of its
 * scheduler-specific id.
 */
static int
sched_kill_task_by_id(int id)
{	

	struct node *tempo = first;
	while (tempo != NULL)
	{
		if(tempo->id == id){
			kill(tempo->pid,SIGKILL);
			return tempo->id;
		};
		tempo=tempo->next;
	}
	return -ENOSYS;
}


/* Create a new task.  */
static void
sched_create_task(char *executable)
{
	pid_t pid;
	int status;
	pid = fork();
        if (pid == 0){
            char *newargv[] = {executable, NULL, NULL, NULL};
			char *newenviron[] = {NULL};
            raise(SIGSTOP);
            execve(executable, newargv, newenviron);
        };
	++tasknum;
        printf("tasknum is %d \n", tasknum);
        enqueue(tasknum, pid, executable);
	waitpid(pid, &status, WUNTRACED);
}

/* Process requests by the shell.  */
static int
process_request(struct request_struct *rq)
{
	switch (rq->request_no) {
		case REQ_PRINT_TASKS:
			sched_print_tasks();
			return 0;

		case REQ_KILL_TASK:
			return sched_kill_task_by_id(rq->task_arg);

		case REQ_EXEC_TASK:
			sched_create_task(rq->exec_task_arg);
			return 0;

		default:
			return -ENOSYS;
	}
}

/* SIGALRM handler: Gets called whenever an alarm goes off.
 * The time quantum of the currently executing process has expired,
 * so send it a SIGSTOP. The SIGCHLD handler will take care of
 * activating the next in line.
 */
static void
sigalrm_handler(int signum)
{
    if(first != NULL) kill(first->pid, SIGSTOP);
}


/* SIGCHLD handler: Gets called whenever a process is stopped,
 * terminated due to a signal, or exits gracefully.
 *
 * If the currently executing task has been stopped,
 * it means its time quantum has expired and a new one has
 * to be activated.
 */
static void
sigchld_handler(int signum)
{
	pid_t p;
    int status;
    do
    {
        p = waitpid(-1, &status,WUNTRACED | WNOHANG | WCONTINUED);
        if (p < 0)
		{
            perror("waitpid");
            exit(1);
        };
		if(p == 0) return;
	if(WIFCONTINUED(status)) return;

	explain_wait_status(p, status);

        if (WIFEXITED(status))
		{
			//printf("exiiiiiiit %d\n",p);
            		dequeue();
            		if (empty()) exit(0);
            		alarm(SCHED_TQ_SEC);
            		kill(first->pid, SIGCONT);
        };
	if (WIFSIGNALED(status)){
		struct node *prev = NULL, *curr = first;
		int flag = 1;
		//sched_print_tasks();
		while ((curr != NULL)&&(flag)){
			if (curr->pid == p){
				if (first == curr) first = first->next;
				if (last == curr) last = prev;
				else prev->next = curr->next;
				free(curr);
				flag = 0;
				if (first == NULL) last = NULL;
			};
			prev = curr;
			curr = curr->next;
		};
		if (empty()) exit(0);
	};
        if (WIFSTOPPED(status))
		{
			//printf("stoooooooooop %d\n",p);
            enqueue(first->id, first->pid, first->name);
	    dequeue();
	    //sched_print_tasks();
            alarm(SCHED_TQ_SEC);
            kill(first->pid, SIGCONT);

        };

	}while(p > 0);
    
}

/* Disable delivery of SIGALRM and SIGCHLD. */
static void
signals_disable(void)
{
	sigset_t sigset;

	sigemptyset(&sigset);
	sigaddset(&sigset, SIGALRM);
	sigaddset(&sigset, SIGCHLD);
	if (sigprocmask(SIG_BLOCK, &sigset, NULL) < 0) {
		perror("signals_disable: sigprocmask");
		exit(1);
	}
}

/* Enable delivery of SIGALRM and SIGCHLD.  */
static void
signals_enable(void)
{
	sigset_t sigset;

	sigemptyset(&sigset);
	sigaddset(&sigset, SIGALRM);
	sigaddset(&sigset, SIGCHLD);
	if (sigprocmask(SIG_UNBLOCK, &sigset, NULL) < 0) {
		perror("signals_enable: sigprocmask");
		exit(1);
	}
}


/* Install two signal handlers.
 * One for SIGCHLD, one for SIGALRM.
 * Make sure both signals are masked when one of them is running.
 */
static void
install_signal_handlers(void)
{
	sigset_t sigset;
	struct sigaction sa;

	sa.sa_handler = sigchld_handler;
	sa.sa_flags = SA_RESTART;
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGCHLD);
	sigaddset(&sigset, SIGALRM);
	sa.sa_mask = sigset;
	if (sigaction(SIGCHLD, &sa, NULL) < 0) {
		perror("sigaction: sigchld");
		exit(1);
	}

	sa.sa_handler = sigalrm_handler;
	if (sigaction(SIGALRM, &sa, NULL) < 0) {
		perror("sigaction: sigalrm");
		exit(1);
	}

	/*
	 * Ignore SIGPIPE, so that write()s to pipes
	 * with no reader do not result in us being killed,
	 * and write() returns EPIPE instead.
	 */
	if (signal(SIGPIPE, SIG_IGN) < 0) {
		perror("signal: sigpipe");
		exit(1);
	}
}

static void
do_shell(char *executable, int wfd, int rfd)
{
	char arg1[10], arg2[10];
	char *newargv[] = { executable, NULL, NULL, NULL };
	char *newenviron[] = { NULL };

	sprintf(arg1, "%05d", wfd);
	sprintf(arg2, "%05d", rfd);
	newargv[1] = arg1;
	newargv[2] = arg2;

	raise(SIGSTOP);
	execve(executable, newargv, newenviron);

	/* execve() only returns on error */
	perror("scheduler: child: execve");
	exit(1);
}

/* Create a new shell task.
 *
 * The shell gets special treatment:
 * two pipes are created for communication and passed
 * as command-line arguments to the executable.
 */
static pid_t
sched_create_shell(char *executable, int *request_fd, int *return_fd)
{
	pid_t p;
	int pfds_rq[2], pfds_ret[2];

	if (pipe(pfds_rq) < 0 || pipe(pfds_ret) < 0) {
		perror("pipe");
		exit(1);
	}

	p = fork();
	if (p < 0) {
		perror("scheduler: fork");
		exit(1);
	}

	if (p == 0) {
		/* Child */
		close(pfds_rq[0]);
		close(pfds_ret[1]);
		do_shell(executable, pfds_rq[1], pfds_ret[0]);
		assert(0);
	}
	/* Parent */
	close(pfds_rq[1]);
	close(pfds_ret[0]);
	*request_fd = pfds_rq[0];
	*return_fd = pfds_ret[1];
	return p;
}

static void
shell_request_loop(int request_fd, int return_fd)
{
	int ret;
	struct request_struct rq;

	/*
	 * Keep receiving requests from the shell.
	 */
	for (;;) {
		if (read(request_fd, &rq, sizeof(rq)) != sizeof(rq)) {
			perror("scheduler: read from shell");
			fprintf(stderr, "Scheduler: giving up on shell request processing.\n");
			break;
		}

		signals_disable();
		ret = process_request(&rq);
		signals_enable();

		if (write(return_fd, &ret, sizeof(ret)) != sizeof(ret)) {
			perror("scheduler: write to shell");
			fprintf(stderr, "Scheduler: giving up on shell request processing.\n");
			break;
		}
	}
}

int main(int argc, char *argv[])
{
	int nproc,i;
	pid_t pid, shellpid;
	/* Two file descriptors for communication with the shell */
	static int request_fd, return_fd;

	/* Create the shell. */
	shellpid = sched_create_shell(SHELL_EXECUTABLE_NAME, &request_fd, &return_fd);
	/* TODO: add the shell to the scheduler's tasks */
	enqueue(0, shellpid, "Shell");
	/*
	 * For each of argv[1] to argv[argc - 1],
	 * create a new child process, add it to the process list.
	 */
	for(i=1; i<argc; ++i){
        pid = fork();
        if (pid == 0){
            char *newargv[] = {argv[i], NULL, NULL, NULL};
            char *newenviron[] = {NULL};
            raise(SIGSTOP);
            execve(argv[i], newargv, newenviron);
        };
        enqueue(i, pid, argv[i]);
    };
	nproc = argc-1; /* number of proccesses goes here */
	tasknum = nproc;

	/* Wait for all children to raise SIGSTOP before exec()ing. */
	wait_for_ready_children(nproc+1);


	/* Install SIGALRM and SIGCHLD handlers. */
	install_signal_handlers();

	if (nproc < 0) {
		fprintf(stderr, "Scheduler: No tasks. Exiting...\n");
		exit(1);
	}
	alarm(SCHED_TQ_SEC);
	kill(shellpid, SIGCONT);

	shell_request_loop(request_fd, return_fd);
	

	/* Now that the shell is gone, just loop forever
	 * until we exit from inside a signal handler.
	 */
	while (pause())
		;

	/* Unreachable */
	fprintf(stderr, "Internal error: Reached unreachable point\n");
	return 1;
}
