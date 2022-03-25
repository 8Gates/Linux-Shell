// Author: Chad D. Smith
// Shell for Linux. Provides prompts for running commands using exec() family
// functions and built in commands such as exit, cd and status. Allows variable expansion of $$ 
// to the shell's PID. Supports input and output redirection. Supports running commands in 
// the foreground and background. Tracks all running processes and notifies user of abnormal termination.
// Implements customer signal handlers for SIGINT and SIGTSTP.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

int foregroundOnly = 0;
int sigTSTPChange = 0;
int lfStatus = -1234;
int bgStatus;
int bgRunning[1000];

// com stucture built from shell user's input command
struct command {
    char* args[512];
    char input[128];
    char output[128];
    int background;
    int numArgs;
};

// redirect stdin to com.input file, return 1 if error occurs 
int inputRedirection(char* input) {
    int newInFD = open(input, O_RDONLY); // open read only
    if (newInFD == -1) {
        perror(input); // print input file name: error statement
        return 1;
    }
    int result = dup2(newInFD, 0); // redirect stdin to com.input file
    close(newInFD);
    if (result == -1) {
        perror(input); // print input file name: error statement
        return 1;
    }
    return 0;
}

// redirect stdout to com.output file, return 1 if error occurs 
int outputRedirection(char* output) {
    // open output file, write only, create or truncate
    int newOutFD = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (newOutFD == -1) {
        perror(output); // print output file name: error statement
        return 1;
    }
    int result = dup2(newOutFD, 1); // Redirect stdout to com.output file
    close(newOutFD);
    if (result == -1) {
        perror(output); // print output file name: error statement 
        return 1;
    }
    return 0;
}

// SIGTSTP handler for parent, foregroundOnly is set to 1 for foreground
// only mode and set to 0 to exit foreground only mode
void tstpHandler(int signo) {
    // enter foreground only mode
    if (foregroundOnly == 0) {
        foregroundOnly = 1;
    }
    else {
        // exit foreground only mode 
        foregroundOnly = 0;
    }
    sigTSTPChange = 1;
}

// gets user command, runs forked child with execvp in foreground or background, I/O redirection enabled
int runShell() {
    char* arguments[512];
    char* line = NULL;
    size_t len = 2048;
    int runInBackground = 0;
    ssize_t nread;

    // instantiate and install foreground only mode handler
    struct sigaction foregroundMode = { 0 };
    foregroundMode.sa_handler = tstpHandler;
    sigfillset(&foregroundMode.sa_mask); // Block all catchable signals while handle_SIGINT is running
    foregroundMode.sa_flags = 0; // // No flags set
    sigaction(SIGTSTP, &foregroundMode, NULL);
    if (sigTSTPChange == 1 && foregroundOnly == 1) {
        printf("Entering foreground-only mode (& is now ignored)\n");
        fflush(stdout);
        sigTSTPChange = 0;
    }
    else if (sigTSTPChange == 1 && foregroundOnly == 0) {
        printf("Exiting foreground-only mode\n");
        fflush(stdout);
        sigTSTPChange = 0;
    }
    // instantiate and install parent ignore SIGINT handler
    struct sigaction sigIgnore = { 0 };
    sigIgnore.sa_handler = SIG_IGN;
    sigaction(SIGINT, &sigIgnore, NULL);

    // instantiate a command and initialize data members 
    struct command com;
    for (int i = 0; i < 512; i++) {
        com.args[i] = NULL;
    }
    strcpy(com.input, "");
    strcpy(com.output, "");
    com.background = 0;
    com.numArgs = 0;

    // checking status of background processes not yet verified to have exited
    int childStatus;
    int testPID;
    for (int ind = 0; ind < (sizeof(bgRunning) / sizeof(int)); ind++) {
        if (bgRunning[ind] == 0) {
            continue;
        }
        testPID = waitpid(bgRunning[ind], &childStatus, WNOHANG); // returns 0 when PID still running
        if (testPID != 0) {
            if (WIFEXITED(childStatus)) { // returns true if the child was terminated normally
                printf("background pid %d is done. exit value %d\n", bgRunning[ind], WEXITSTATUS(childStatus));
                fflush(stdout);
                bgRunning[ind] = 0; // remove PID from bgRunning as it has exited
            }
            else if (WIFSIGNALED(childStatus)) { // returns true if the child was terminated abnormally
                printf("background pid %d is done: terminated by signal %d\n", bgRunning[ind], WTERMSIG(childStatus));
                fflush(stdout);
                bgRunning[ind] = 0; // remove PID from bgRunning as it has exited
            }
        }
    }

    // prompt command, get input and remove \n
    printf(":");
    fflush(stdout);
    nread = getline(&line, &len, stdin);
    // getline returns -1 if interrupted by signal handler functions, clear stdin error and 
    // get next user command
    if (nread == -1) {
        clearerr(stdin);
        printf("\n");
        fflush(stdout);
        return -1;
    }
    char* newline = strchr(line, '\n');
    if (newline)
        *newline = 0;

    // variable expansion: create duplicate line, replace instances of 
    // $$ with %d in duplicate line and then string print in to line
    int expand = 0;
    pid_t shellPID = getpid();
    for (int i = 0; i < nread; i++) {
        if (line[i] == '$' && expand == 0) {
            expand = 1;
        }
        else if (line[i] == '$' && expand == 1) {
            expand = 0;
            char* lineDup = strdup(line);
            lineDup[i - 1] = '%';
            lineDup[i] = 'd';
            sprintf(line, lineDup, shellPID);
            free(lineDup);
        }
    }

    //get tokens of user input and store in arguments array
    char* token = strtok(line, " \t");
    int i = 0;              // index for arguments array
    int argIndex = 1000;    // com's argument index, 1000 indicates no args found
    while (token != NULL) {
        arguments[i] = token;
        // ignore comment lines by returning 0
        if (arguments[i][0] == '#' && i == 0) {
            printf("\n");
            fflush(stdout);
            return 0;
        }
        else if (*(arguments[0] + 0) == '#' && i == 0) {
            printf("\n");
            fflush(stdout);
            return 0;
        }
        // record the end of the arguments index when redirections are found
        if (strcmp(arguments[i], "<") == 0 || strcmp(arguments[i], ">") == 0) {
            if (argIndex == 1000) {
                argIndex = i - 1;
            }
        }
        token = strtok(NULL, " ");
        i++;
    }

    // identify if process should run in the background
    i--;
    if (strcmp(arguments[i], "&") == 0) {
        com.background = 1;
    }
    else {
        com.background = 0;
    }

    // locates last element index for args and stores the
    // index+1 in numArgs for our com structure
    int z;
    if (argIndex == 1000) {
        z = i;
        com.numArgs = z + 1;
    }
    else {
        z = argIndex;
        com.numArgs = z + 1;
    }
    // stores arguments[0:z] in com.args 
    // index position z stops before tokens <, >, or &
    for (int j = 0; j <= z; j++) {
        if (i == j && strcmp(arguments[i], "&") == 0) {
            continue;
        }
        else {
            com.args[j] = arguments[j];
        }
    }
    // stores token following redirection requests < > in input and output
    // of our com structure 
    for (int j = 0; j < i; j++) {
        if (strcmp(arguments[j], "<") == 0) {
            int x = j + 1; //index of output file
            strcpy(com.input, arguments[x]);
        }
        if (strcmp(arguments[j], ">") == 0) {
            int y = j + 1; //index of input file
            strcpy(com.output, arguments[y]);
        }
    }

    // the com structure is now fully built, return if no arguments
    if (com.numArgs == 0) {
        return 0;
    };
 
    // if in foreground mode ignore requested '&'
    if (foregroundOnly == 1) {
        com.background = 0;
    }

    // "exit" entered: kill all processes and exit
    if (strcmp(com.args[0], "exit") == 0) {
        int killReturn;
        // SIGKILL any processes that have yet to terminate
        for (int ind = 0; ind < (sizeof(bgRunning) / sizeof(int)); ind++) {
            if (bgRunning[ind] == 0) {
                continue;
            }
            printf("Attempting to kill %d\n", bgRunning[ind]);
            fflush(stdout);
            killReturn = kill(bgRunning[ind], SIGKILL);
            if (killReturn == -1) {
                printf("Process %d was not killed\n", bgRunning[ind]);
                fflush(stdout);
            }
            else {
                printf("Process %d was killed\n", bgRunning[ind]);
                fflush(stdout);
            }
        }
        exit(0); //exit the shell
    }
    else if (strcmp(com.args[0], "cd") == 0) {
        //with no arguments, "cd" changes to the directory specified in the HOME environment
        //variable. Can take 1 arg and works w/both absolute and relative paths
        if (com.numArgs == 1) {
            chdir(getenv("HOME"));
        }
        else if (com.numArgs == 2) {
            chdir(com.args[1]);
        }
        return 0;
    }
    else if (strcmp(com.args[0], "status") == 0) {
        // prints out either exit status or terminating signal of last foreground
        // process ran by this shell. If run before any foreground command is run,
        // returns 0.
        if (lfStatus == -1234) {
            printf("exit value 0\n");
            fflush(stdout);
        }
        else {
            if (WIFEXITED(lfStatus)) {
                printf("exit value %d\n", WEXITSTATUS(lfStatus));
                fflush(stdout);
            }
            else {
                printf("terminated by signal %d\n", WTERMSIG(lfStatus));
                fflush(stdout);
            }
        }
        return 0;
    }
    else {
        pid_t spawnPid = fork(); // Fork a new child process
        switch (spawnPid) {
        case -1:
            perror("fork()\n");
            exit(1);
            break;
        case 0:;// *** CHILD PROCESS ***
            // child ignore SIGTSTP handler install
            struct sigaction tstpIgnore = { 0 };
            tstpIgnore.sa_handler = SIG_IGN;
            sigaction(SIGTSTP, &tstpIgnore, NULL);
            if (com.background == 0) {
                // foreground child default SIGINT handler install
                struct sigaction sigDefault = { 0 };
                sigDefault.sa_handler = SIG_DFL;
                sigaction(SIGINT, &sigDefault, NULL);
            }
            else {
                // background child ignore SIGINT handler install
                struct sigaction sigDefault = { 0 };
                sigDefault.sa_handler = SIG_IGN;
                sigaction(SIGINT, &sigDefault, NULL);
            }
            // I/0 REDIRECTION 
            //inputRedirection and outputRedirection functions exit 1 if error encountered
            if (strcmp(com.input, "") != 0) { 
                if (inputRedirection(com.input) == 1) // stdin redirect specified
                    exit(1);
            }
            if (strcmp(com.output, "") != 0) { 
                if (outputRedirection(com.output) == 1) // stdout redirect specified
                    exit(1);
            }
            if (com.background == 1 && strcmp(com.input, "") == 0) {
                // background process stdin redirection to /dev/null if not specified 
                if (inputRedirection("/dev/null") == 1)
                    exit(1);
            }
            if (com.background == 1 && strcmp(com.output, "") == 0) {
                // background process stdout redirection to /dev/null if not specified
                if (outputRedirection("/dev/null") == 1)
                    exit(1);
            }
            execvp(com.args[0], com.args); // accepting Vector and searching PATH
            perror("execvp"); // exec only returns on error, print error
            fflush(stdout);
            exit(1);
            break;
        default: // *** PARENT PROCESS ***
            if (com.background == 1) {
                printf("PID %d started in background \n", spawnPid);
                fflush(stdout);
                // store background pid in bgRunning array 
                for (int ind = 0; ind < (sizeof(bgRunning) / sizeof(int)); ind++) {
                    if (bgRunning[ind] == 0) { // located empty index pos for background pid
                        bgRunning[ind] = spawnPid;
                        break;
                    }
                }
                spawnPid = waitpid(spawnPid, &bgStatus, WNOHANG); // background process WNOHANG
            }
            else {
                spawnPid = waitpid(spawnPid, &lfStatus, 0); // foreground process wait for termination
                if (lfStatus == 2 && com.background == 0) {
                    // foreground child terminated Signal Value: 2, Signal Name: SIGINT
                    printf("terminated by signal 2\n");
                    fflush(stdout);
                }
            }
        }
    }
    free(line);
    return 0;
}

int main(void) {
    // run shell until exit signal received
    while (1) {
        runShell();
    }
    return 0;
}