#include <sys/wait.h>
#include "../inc/logging.h"
#include "../inc/anav.h"
#include "../inc/parse.h"
#include "../inc/util.h"

/* Constants */
#define DEBUG 0 /* You can set this to 0 to turn off the debug parse information */
#define STOP_SHELL  0
#define RUN_SHELL   1
#define READ_END 0
#define WRITE_END 1

typedef struct task{
    int task_num;
    int pid;
    char* cmd;
    char** argv;
    int status;
    int type; /* 0 for foreground and 1 for background */
    int exit_code;
} Task;

Task** list = NULL;
int new_task_num = 1;
int suspend = 1;

void block(){
    sigset_t mask;
    sigfillset(&mask);
    sigprocmask(SIG_BLOCK, &mask, NULL);
}

void unblock(){
    sigset_t mask;
    sigfillset(&mask);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
}

/* Extracts information from the wstatus filled by waitpid */
void extract(int wstatus, int* status, int* transition){
    if (WIFEXITED(wstatus)){
        *status = LOG_STATE_FINISHED; 
        *transition = LOG_TERM;
    }
    else if (WIFSIGNALED(wstatus)){
        *status = LOG_STATE_KILLED; 
        *transition = LOG_TERM_SIG;
    }
    else if (WIFSTOPPED(wstatus)){
        *status = LOG_STATE_SUSPENDED; 
        *transition = LOG_SUSPEND;
    }
    else if (WIFCONTINUED(wstatus)){
        *status = LOG_STATE_RUNNING; 
        *transition = LOG_RESUME;
    }
}

/* Stalls the shell until the foreground process changes status */
void foreground(){
    unblock();
    while (suspend == 1);
}

void handler(int sig){
    int wstatus = 0;
    int status = 0;
    int transition = 0;
    int pid = -1;
    int i = 0;
    int run_exists = 0;
    block();
    /* Handles any status change from children */
    if (sig == SIGCHLD){
        /* Reap in a loop until there are no more signals to handle */
        while (1){
            pid = waitpid(-1, &wstatus, WNOHANG | WUNTRACED | WCONTINUED);
            for (i=0;i<new_task_num-1;i++){
                if (list[i] != NULL && list[i]->status == LOG_STATE_RUNNING){
                    run_exists = 1;
                }
            }
            /* If there are no more signals to handle or if there are no running processes then exit the loop */
            if (pid == 0 || (pid == -1 && run_exists == 0)){
                break;
            }
            else{
                /* Handle the signal and update the process' status */
                extract(wstatus, &status, &transition); 
                for (i=0;i<new_task_num-1;i++){
                    if (list[i] != NULL){
                        if (list[i]->pid == pid){
                            if (list[i]->type == 0){
                                suspend = 0;
                            }
                            list[i]->status = status;
                            list[i]->exit_code = WEXITSTATUS(wstatus);
                            if (transition == LOG_RESUME){
                                list[i]->type = 0;
                            }
                            log_anav_status_change(i+1, pid, list[i]->type, list[i]->cmd, transition);
                            if (transition == LOG_RESUME){
                                foreground();
                            }
                            run_exists = 0;
                            break;
                        }
                    }
                }
            }
        }
    }
    /* Handle any keyboard signals */
    else{
        for (i=0;i<new_task_num-1;i++){
            if (list[i] != NULL){
                if (list[i]->type == 0 && list[i]->status == LOG_STATE_RUNNING){
                    kill(list[i]->pid, sig);
                    if (sig == SIGINT) log_anav_ctrl_c();
                    else if (sig == SIGTSTP) log_anav_ctrl_z();
                    break;
                }
            }
        }
    }
    unblock();
}

/* The entry of your text processor program */
int main() {
    char *cmd = NULL;
    int do_run_shell = RUN_SHELL;
    int num_tasks = 0;
    int size = 10;
    int i = 0;
    Task *t = NULL;
    int pid = 0;
    char path[MAXLINE+10] = "";
    int pipefd[2] = {0};
    int fd = 0;
    struct sigaction sa = {0};
    struct sigaction childsa = {0};

    list = malloc(size*sizeof(Task*));
    if (list == NULL) exit(1);
    for (i=0;i<size;i++){
        list[i] = malloc(sizeof(Task));
        if (list[i] == NULL) exit(1);
    }

    sa.sa_handler = handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);

    /* Intial Prompt and Welcome */
    log_anav_intro();
    log_anav_help();

  /* Shell looping here to accept user command and execute */
    while (do_run_shell == RUN_SHELL) {
        char *argv[MAXARGS+1] = {0};  /* Argument list */
        Instruction inst = {0};       /* Instruction structure: check parse.h */

        /* Print prompt */
        log_anav_prompt();

        /* Get Input - Allocates memory for the cmd copy */
        cmd = get_input(); 
        /* If the input is whitespace/invalid, get new input from the user. */
        if(cmd == NULL) {
          continue;
        }

        /* Check to see if this is the quit built-in */
        if(strncmp(cmd, "quit", 4) == 0) {
          /* This is a match, so we'll set the main loop to exit when you finish processing it */
          do_run_shell = STOP_SHELL;
          /* Note: You will need to print a message when quit is entered, 
           *       you can do it here, in your functions, or at the end of main.
           */
          log_anav_quit();
          continue;
        }

        if (strncmp(cmd, "help", 4) == 0){
            log_anav_help();
            continue;
        }

        if (strncmp(cmd, "list", 4) == 0){
            block();
            log_anav_num_tasks(num_tasks);
            for (i=0;i<new_task_num-1;i++){
                if (list[i] != NULL){
                    t = list[i];
                    log_anav_task_info(t->task_num, t->status, t->exit_code, t->pid, t->cmd);
                }
            }
            unblock();
            continue;
        }

        /* Parse the Command and Populate the Instruction and Arguments */
        initialize_command(&inst, argv);    /* initialize arg lists and instruction */
        parse(cmd, &inst, argv);            /* call provided parse() */

        if (DEBUG) {  /* display parse result, redefine DEBUG to turn it off */
          debug_print_parse(cmd, &inst, argv, "main (after parse)");
        }

        /*.===============================================.
         *| Add your code below this line to continue. 
         *| - The command has been parsed and you have cmd, inst, and argv filled with data.
         *| - Very highly recommended to start calling your own functions after this point.
         *o===============================================*/

        if (strncmp(inst.instruct, "purge", 5) == 0){
            block();
            if (inst.id1 <= new_task_num-1) t = list[inst.id1-1];
            if (inst.id1 > new_task_num-1 || list[inst.id1-1] == NULL){
                log_anav_task_num_error(inst.id1);
            }
            else if (t->status == LOG_STATE_RUNNING || t->status == LOG_STATE_SUSPENDED){
                log_anav_status_error(t->task_num, t->status);
            }
            else{
                free(t->cmd);
                free_argv(t->argv); 
                free(t);
                list[inst.id1-1] = NULL; 
                num_tasks--;
                log_anav_purge(inst.id1);
            }
            unblock();
            continue;
        }

        if (strncmp(inst.instruct, "exec", 4) == 0 || strncmp(inst.instruct, "bg", 2) == 0 || strncmp(inst.instruct, "pipe", 4) == 0){
            if (inst.id1 <= new_task_num-1) t = list[inst.id1-1];
            if (inst.id1 > new_task_num-1 || t == NULL){
                log_anav_task_num_error(inst.id1);
                continue;
            }
            else if (t->status == LOG_STATE_RUNNING || t->status == LOG_STATE_SUSPENDED){
                log_anav_status_error(t->task_num, t->status);
            }
            else{
                /* Set the type to background */
                if (strncmp(inst.instruct, "bg", 2) == 0 || strncmp(inst.instruct, "pipe", 4) == 0){
                    t->type = 1;
                }
                if (strncmp(inst.instruct, "pipe", 4) == 0){
                    if (inst.id1 == inst.id2){
                        log_anav_pipe_error(inst.id1);
                        continue;
                    }
                    if (pipe(pipefd) == -1){
                        log_anav_file_error(inst.id1, LOG_FILE_PIPE);
                        continue;
                    }
                    log_anav_pipe(inst.id1, inst.id2);
                }
                block();
                t->status = LOG_STATE_RUNNING;
                /* First child */
                pid = fork(); 
                if (pid == 0){
                    setpgid(0,0);
                    /* Reset signal handlers */
                    childsa.sa_handler = SIG_DFL;
                    sigaction(SIGINT, &childsa, NULL);
                    sigaction(SIGTSTP, &childsa, NULL);
                    sigaction(SIGCHLD, &childsa, NULL);

                    /* Create pipe */
                    if (strncmp(inst.instruct, "pipe", 4) == 0){
                        close(pipefd[READ_END]);
                        dup2(pipefd[WRITE_END], STDOUT_FILENO);
                    }
                    else{
                        /* Open infile */
                        if (inst.infile != NULL){
                            fd = open(inst.infile, O_RDONLY, 0644);
                            if (fd == -1){
                                log_anav_file_error(t->task_num, inst.infile);
                                exit(1);
                            }
                            dup2(fd, STDIN_FILENO);
                            log_anav_redir(t->task_num, LOG_REDIR_IN, inst.infile);
                        }
                        /* Write to infile */
                        if (inst.outfile != NULL){
                            fd = open(inst.outfile, O_WRONLY | O_TRUNC | O_CREAT, 0644);
                            if (fd == -1){
                                log_anav_file_error(t->task_num, inst.outfile);
                                exit(1);
                            }
                            dup2(fd, STDOUT_FILENO);
                            log_anav_redir(t->task_num, LOG_REDIR_OUT, inst.outfile);
                        }
                    }

                    unblock();

                    /* Attempt to exec with both paths */
                    strncpy(path, "./", MAXLINE+10);
                    strncat(path, (t->argv)[0], MAXLINE); 
                    execv(path, t->argv);

                    strncpy(path, "/usr/bin/", MAXLINE+10);
                    strncat(path, (t->argv)[0], MAXLINE); 
                    execv(path, t->argv);

                    log_anav_exec_error(t->cmd);
                    exit(1);
                }
                else{
                    t->pid = pid;
                    if (strncmp(inst.instruct, "pipe", 4) == 0){
                        t->status = LOG_STATE_RUNNING;
                        log_anav_status_change(t->task_num, t->pid, LOG_BG, t->cmd, LOG_START);

                        if (inst.id2 <= new_task_num-1) t = list[inst.id2-1];
                        if (inst.id2 > new_task_num-1 || t == NULL){
                            log_anav_task_num_error(inst.id2);
                        }
                        else if (t->status == LOG_STATE_RUNNING || t->status == LOG_STATE_SUSPENDED){
                            log_anav_status_error(t->task_num, t->status);
                        }
                        else{
                            block();
                            /* Second child */
                            pid = fork();
                            if (pid == 0){
                                setpgid(0,0);
                                childsa.sa_handler = SIG_DFL;
                                sigaction(SIGINT, &childsa, NULL);
                                sigaction(SIGTSTP, &childsa, NULL);
                                sigaction(SIGCHLD, &childsa, NULL);

                                close(pipefd[WRITE_END]);
                                dup2(pipefd[READ_END], STDIN_FILENO);

                                unblock();

                                strncpy(path, "./", MAXLINE+10);
                                strncat(path, (t->argv)[0], MAXLINE); 
                                execv(path, t->argv);

                                strncpy(path, "/usr/bin/", MAXLINE+10);
                                strncat(path, (t->argv)[0], MAXLINE); 
                                execv(path, t->argv);

                                log_anav_exec_error(cmd);
                                exit(1);
                            }
                            else{
                                t->pid = pid;
                                close(pipefd[0]);
                                close(pipefd[1]);
                                log_anav_status_change(t->task_num, t->pid, LOG_FG, t->cmd, LOG_START);
                                /* Stall until foreground process is updated */
                                foreground();
                            }
                        }
                    }
                    else if (strncmp(inst.instruct, "exec", 4) == 0){
                        log_anav_status_change(t->task_num, t->pid, LOG_FG, t->cmd, LOG_START);
                        /* Stall until foreground process is updated */
                        foreground();
                    }
                    else{
                        log_anav_status_change(t->task_num, t->pid, LOG_BG, t->cmd, LOG_START);
                        unblock();
                    }
                }
            }
            continue;
        }

        if (strncmp(inst.instruct, "kill", 4) == 0 || strncmp(inst.instruct, "suspend", 7) == 0 || strncmp(inst.instruct, "resume", 6) == 0){
            if (inst.id1 <= new_task_num-1) t = list[inst.id1-1];
            if (inst.id1 > new_task_num-1 || t == NULL){
                log_anav_task_num_error(inst.id1);
            }
            else if (t->status == LOG_STATE_READY || t->status == LOG_STATE_FINISHED || t->status == LOG_STATE_KILLED){
                log_anav_status_error(t->task_num, t->status);
            }
            else{
                if (strncmp(inst.instruct, "kill", 4) == 0){
                    kill(t->pid, SIGINT);
                    log_anav_sig_sent(LOG_CMD_KILL, t->task_num, pid);
                }
                else if (strncmp(inst.instruct, "suspend", 7) == 0){
                    kill(t->pid, SIGTSTP);
                    log_anav_sig_sent(LOG_CMD_SUSPEND, t->task_num, pid);
                }
                else if (strncmp(inst.instruct, "resume", 6) == 0){
                    kill(t->pid, SIGCONT);
                    log_anav_sig_sent(LOG_CMD_RESUME, t->task_num, pid);
                }
            }
            continue;
        }
        
        /* Create task and add it to the list */
        block();
        Task task = {new_task_num, 0, string_copy(cmd), clone_argv(argv), LOG_STATE_READY, 0, 0};
        log_anav_task_init(task.task_num, task.cmd);
        /* Double the size of the list if it is full */
        if (new_task_num-1 == size){
            size *= 2;
            list = realloc(list, (size)*(sizeof(Task*)));
            if (list == NULL) exit(1);
            for (i=size/2;i<size;i++){
                list[i] = malloc(sizeof(Task));
                if (list[i] == NULL) exit(1);
            }
        }
        *list[task.task_num-1] = task;
        num_tasks++;
        new_task_num++;
        unblock();

        


        /*.===============================================.
         *| After your code: We cleanup before Looping to the next command.
         *| free_command WILL free the cmd, inst and argv data.
         *| Make sure you've copied any needed information to your Task first.
         *| Hint: You can use the util.c functions for copying this information.
         *o===============================================*/
        free_command(cmd, &inst, argv);
        cmd = NULL;
  }

  return 0;
}
