#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>

struct Historyinfo {
    char command[100];
    pid_t ppid;
    struct timeval start_time;
    struct timeval end_time;
    long dur;
};

struct shmmem {
    int i;
    struct Historyinfo prehis[100];
    sem_t *mutex;
} *temp;

int shm_fd;

struct shmmem* setup() {
    char *shm_name = "my_shared_history";
    shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("Shared memory failed");
        exit(1);
    }
    
    ftruncate(shm_fd, sizeof(struct shmmem));
    
    struct shmmem *sh_ptr = mmap(NULL, sizeof(struct shmmem), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (sh_ptr == MAP_FAILED) {
        perror("Mapping Failed");
        exit(1);
    }
    
    return sh_ptr;
}

void cleanup() {
    sem_destroy(temp->mutex);
    munmap(temp, sizeof(struct shmmem));
    shm_unlink("my_shared_history");
    close(shm_fd);
}

char* timevalToStandardTime(struct timeval time) {
    time_t rtime = time.tv_sec;
    struct tm* timeinfo = localtime(&rtime);
    char* timestr = asctime(timeinfo);
    
    size_t len = strlen(timestr);
    if (len > 0 && timestr[len - 1] == '\n') {
        timestr[len - 1] = '\0';
    }
    
    return timestr;
}

void write_history(char *com, int ppid) {
    if (ppid == 0) return;

    sem_wait(temp->mutex);
    
    strcpy(temp->prehis[temp->i].command, com);
    gettimeofday(&temp->prehis[temp->i].start_time, NULL);
    temp->prehis[temp->i].ppid = ppid;

    temp->i++;
    
    sem_post(temp->mutex);
}

void display_history() {
    sem_wait(temp->mutex); // Lock before reading history
    for (int i = 0; i < temp->i; i++) {
        printf("command : %s \t process id : %d \t duration : %ld \t start time : %s\n",
               temp->prehis[i].command,
               temp->prehis[i].ppid,
               temp->prehis[i].dur,
               timevalToStandardTime(temp->prehis[i].start_time));
    }
    sem_post(temp->mutex); // Unlock after reading history
}

void handle_exit(int signum) {
    display_history();
    cleanup();
    exit(0);
}

// Function to execute piped commands
void execute_piped_commands(char *commands[], int num_commands) {
    int pipefds[2 * (num_commands - 1)]; // Pipes between commands
    
    for (int i = 0; i < num_commands - 1; i++) {
        if (pipe(pipefds + i * 2) == -1) {
            perror("Pipe failed");
            exit(EXIT_FAILURE);
        }
    }

    int command_count = 0;
    while (command_count < num_commands) {
        pid_t pid = fork();

        if (pid == 0) { // Child process
            if (command_count < num_commands - 1) {
                // Redirect stdout to the pipe
                if (dup2(pipefds[command_count * 2 + 1], STDOUT_FILENO) == -1) {
                    perror("dup2 failed");
                    exit(EXIT_FAILURE);
                }
            }
            if (command_count > 0) {
                // Redirect stdin to the pipe
                if (dup2(pipefds[(command_count - 1) * 2], STDIN_FILENO) == -1) {
                    perror("dup2 failed");
                    exit(EXIT_FAILURE);
                }
            }
            
            // Close all pipe file descriptors in the child process
            for (int i = 0; i < 2 * (num_commands - 1); i++) {
                close(pipefds[i]);
            }

            // Execute the command
            char *inputc[100];
            int cnt = 0;
            char *tok = strtok(commands[command_count], " ");
            while (tok != NULL) {
                inputc[cnt++] = tok;
                tok = strtok(NULL, " ");
            }
            inputc[cnt] = NULL;

            execvp(inputc[0], inputc);
            perror("Command execution failed");
            exit(EXIT_FAILURE);
        } else if (pid < 0) {
            perror("Fork failed");
            exit(EXIT_FAILURE);
        }
        command_count++;
    }

    // Close all pipe file descriptors in the parent process
    for (int i = 0; i < 2 * (num_commands - 1); i++) {
        close(pipefds[i]);
    }

    // Wait for all child processes
    for (int i = 0; i < num_commands; i++) {
        wait(NULL);
    }
}

void execute_command(char *command) {
    char *inputc[100];
    int cnt = 0;

    // Split the command by pipes "|"
    char *piped_commands[100];
    int pipe_count = 0;
    char *pipe_token = strtok(command, "|");
    while (pipe_token != NULL) {
        piped_commands[pipe_count++] = pipe_token;
        pipe_token = strtok(NULL, "|");
    }

    if (pipe_count > 1) {
        // Handle piped commands
        execute_piped_commands(piped_commands, pipe_count);
        return;
    }

    // If no pipes are found, continue with single command execution
    char *tok = strtok(command, " ");
    while (tok != NULL) {
        inputc[cnt++] = tok;
        tok = strtok(NULL, " ");
    }
    
    inputc[cnt] = NULL;

    if (strcmp(inputc[0], "cd") == 0) {
        if (cnt > 1) {
            if (chdir(inputc[1]) != 0) {
                perror("cd failed");
            }
        } else {
            fprintf(stderr, "cd: missing argument\n");
        }
        return;
    }

    if (strcmp(inputc[0], "mkdir") == 0) {
        if (cnt > 1) {
            if (mkdir(inputc[1], 0755) != 0) {
                perror("mkdir failed");
            }
        } else {
            fprintf(stderr, "mkdir: missing argument\n");
        }
        return;
    }

    if (strcmp(inputc[0], "history") == 0) {
        display_history();
        return;
    }

    pid_t pid = fork();
    
    if (pid == 0) { // Child process
        execvp(inputc[0], inputc);
        perror("Command not executed");
        exit(1);
    } else if (pid > 0) { // Parent process
        write_history(command, pid); // Log command in history
        wait(NULL); // Wait for child to finish

        // Calculate duration
        struct timeval end_time;
        gettimeofday(&end_time, NULL);
       
        long duration = ((end_time.tv_sec - temp->prehis[temp->i - 1].start_time.tv_sec) * 1000 +
                        (end_time.tv_usec - temp->prehis[temp->i - 1].start_time.tv_usec) / 1000);
                        
        temp->prehis[temp->i - 1].end_time = end_time; // Store end time
        temp->prehis[temp->i - 1].dur = duration; // Store duration
    } else {
        perror("Fork failed");
    }
}

void take_input() {
    char input[100];
   
    while (1) {
        char cwd[1024];
        getcwd(cwd, sizeof(cwd));
       
        printf("meechan@UNI:%s$ ", cwd); // Custom prompt
       
        if (fgets(input, sizeof(input), stdin) == NULL) {
            break; // Exit on EOF
        }
       
        input[strcspn(input, "\n")] = 0; // Remove newline character
       
        execute_command(input);
       
        // Reset input buffer
        memset(input, 0, sizeof(input));
    }
}

int main() {
    signal(SIGINT, handle_exit); // Handle Ctrl+C

    temp = setup();
    temp->i = 0;
    temp->mutex = malloc(sizeof(sem_t));

    if (!temp->mutex || sem_init(temp->mutex, 1, 1)) {
        perror("Malloc or Semaphore initialization failed");
        exit(1);
    }

    take_input(); // Start taking input

    cleanup(); // Clean up resources
}