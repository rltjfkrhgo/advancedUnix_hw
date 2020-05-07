// 201624416-KIM-GISEO.c
// Advanced Unix Take-Home Midterm Exam, Spring 2020.
// KIM GISEO, ID# 201624416

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <pthread.h>
#include <ncurses.h>
#include <semaphore.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>

#define ID_SIZE 16      // the max length of user ID
#define BUFF_SIZE 1024  // the max length a single message
#define MAX_USER 18     // the number of users can join chatting room

// struct of a single message
typedef struct
{
    int  id;
    char sender[ID_SIZE];
    char msg[BUFF_SIZE];
    bool isValid;
} Message;

// logging in users
typedef struct
{
    int  numOfUser;
    char user[MAX_USER][ID_SIZE];
} LoginUser;

// struct for shared memory
typedef struct
{
    Message   message;
    int       readCount;
    int       nextMsgID;
    LoginUser loginUser;
    sem_t*    sem;
} ShmStruct;

int        shmid;
ShmStruct* shmPtr;  // *shmPtr points to the shared memory segment
sem_t*     sem;     // semaphore to synchronize the shared memory among processes

WINDOW* outputScr;
WINDOW* inputScr;
WINDOW* loginScr;
WINDOW* timeScr;
WINDOW* outputBox;

Message   buffSend;   // message to send
Message   buffRecv;   // message to receive
LoginUser loginUser;  // local information of logging in users

pthread_mutex_t mutexSend;
pthread_cond_t notFullSend;
pthread_cond_t notEmptySend;

pthread_mutex_t mutexRecv;
pthread_cond_t notFullRecv;
pthread_cond_t notEmptyRecv;

int isRunning;

void chatInit();
void chatJoin();
void ncursesInit();
void chat();
void chatExit();

void* fetchMessageFromShmThread();
void* writeMessageToShmThread();
void* displayMessageThread();
void* getInputMessageThread();
void* displayLoginUserThread();
void* displayTimeThread();

void sigintHandler(int signo);

int main(int argc, char* argv[])
{
    // if did not put the user ID when running
    // inform how to use
    if(argc != 2)
    {
        fprintf(stderr, "[Usage] : ./chat <userID> \n");
        return -1;
    }

    // get User ID
    strcpy(buffSend.sender, argv[1]);

    chatInit();
    chatJoin();
    ncursesInit();
    chat();
    chatExit();

    return 0;
}

void chatInit()
{
    // get the shmid of shared memory
    shmid = shmget((key_t)201624416, sizeof(ShmStruct), 0644);

    // if this process is the FIRST
    // create the shared memory and attach it,
    // initialize variables in shmPtr and create the semaphore
    if(shmid < 0)
    {
        shmid = shmget((key_t)201624416, sizeof(ShmStruct), 0644|IPC_CREAT|IPC_EXCL);
        shmPtr = (ShmStruct*)shmat(shmid, NULL, 0644);

        shmPtr->message.id = 0;
        shmPtr->message.isValid = false;
        shmPtr->nextMsgID = 1;
        shmPtr->loginUser.numOfUser = 0;
        shmPtr->readCount = shmPtr->loginUser.numOfUser;

        // create the named semaphore
        // initial value is ONE!
        shmPtr->sem = sem_open("sem", O_CREAT|O_EXCL, 0644, 1);
        if(shmPtr->sem == SEM_FAILED)
        {
            perror("sem_open(\"sem\", O_CREATE) : ");
            exit(-1);
        }
    }

    // already exsisted, just attach
    else
    {
        shmPtr = (ShmStruct*)shmat(shmid, NULL, 07644);
    }

    // open the semaphore
    sem = sem_open("sem", 0);
    if(sem == SEM_FAILED)
    {
        perror("sem_open(\"sem\", 0) : ");
        exit(-1);
    }

    // initialize mutexes and conditions
    pthread_mutex_init(&mutexSend, NULL);
    pthread_cond_init(&notFullSend, NULL);
    pthread_cond_init(&notEmptySend, NULL);

    pthread_mutex_init(&mutexRecv, NULL);
    pthread_cond_init(&notFullRecv, NULL);
    pthread_cond_init(&notEmptyRecv, NULL);
}

void chatJoin()
{
    /* ========== start of the shared memory section ========== */
    // check shmPtr->message is not vaild
    while(1)
    {
        // LOCK!!
        sem_wait(sem);

        if(shmPtr->message.isValid == false)
            break;

        // UNLOCK!!
        sem_post(sem);
        usleep(100);
    }

    // the semaphore is still LOCKED!!

    // before join chatting room
    // check the number of users
    if(shmPtr->loginUser.numOfUser == MAX_USER)
    {
        // I must unlock the semaphore
        sem_post(sem);
        fprintf(stderr, "chatting room is FULL!!\n");
        exit(-1);
    }
    
    // finally ENTER!
    strcpy(shmPtr->loginUser.user[shmPtr->loginUser.numOfUser], buffSend.sender);
    shmPtr->loginUser.numOfUser++;

    // UNLOCK!!
    sem_post(sem);
    /* ========== end of the shared memory section ========== */
}

void ncursesInit()
{
    // start ncurses mode
    initscr();

    // make 4 subwindows
    outputBox = subwin(stdscr, 20, 60,  0,  0);
    inputScr  = subwin(stdscr,  4, 60, 20,  0);
    loginScr  = subwin(stdscr, 20, 20,  0, 60);
    timeScr   = subwin(stdscr,  4, 20, 20, 60);

    // draw boxline of each window
    box(outputBox, ACS_VLINE, ACS_HLINE);
    box(inputScr, ACS_VLINE, ACS_HLINE);
    box(loginScr, ACS_VLINE, ACS_HLINE);
    box(timeScr, ACS_VLINE, ACS_HLINE);

    outputScr = subwin(outputBox, 18, 58, 1, 1);
    scrollok(outputScr, TRUE);

    wrefresh(outputBox);
    wrefresh(inputScr);
    wrefresh(outputScr);
    wrefresh(loginScr);
    wrefresh(timeScr);

    // register signal handler
    signal(SIGINT, sigintHandler);
}

// create threads related to chatting
void chat()
{
    isRunning = 1;

    // inform that I joined
    strcpy(buffSend.msg, "/hello");
    buffSend.isValid = true;

    // no message to receive
    buffRecv.id = 0;
    buffRecv.isValid = false;

    loginUser.numOfUser = 0;

    // create threads and wait for them
    pthread_t thread[6];

    pthread_create(&thread[0], NULL, fetchMessageFromShmThread, NULL);
    pthread_create(&thread[1], NULL, writeMessageToShmThread, NULL);
    pthread_create(&thread[2], NULL, displayMessageThread, NULL);
    pthread_create(&thread[3], NULL, getInputMessageThread, NULL);
    pthread_create(&thread[4], NULL, displayLoginUserThread, NULL);
    pthread_create(&thread[5], NULL, displayTimeThread, NULL);

    for(int i = 0; i < 6; i++)
    {
        pthread_join(thread[i], NULL);
    }
}

void* fetchMessageFromShmThread()
{
    while(isRunning)
    {
        pthread_mutex_lock(&mutexRecv);

        // wait until buffRecv is not valid
        while(buffRecv.isValid == true)
        {
            pthread_cond_wait(&notFullRecv, &mutexRecv);
        }

        /* ========== start of the shared memory section ========== */
        while(1)
        {
            sem_wait(sem);

            // this is a NEW message!!
            if(buffRecv.id != shmPtr->message.id)
                break;

            sem_post(sem);
            usleep(100);
        }

        // FETCH!!
        memcpy(&buffRecv, &(shmPtr->message), sizeof(Message));
        shmPtr->readCount++;

        // if all of users read the message in shm
        // the message is not vaild
        // reset the read count
        if(shmPtr->readCount >= shmPtr->loginUser.numOfUser)
        {
            shmPtr->readCount = 0;
            shmPtr->message.isValid = false;
        }

        sem_post(sem);
        /* ========== end of the shared memory section ========== */

        // notify that buffRecv is not empty to the displayThread
        pthread_mutex_unlock(&mutexRecv);
        pthread_cond_signal(&notEmptyRecv);
    }

    return NULL;
}

void* displayMessageThread()
{
    wprintw(outputScr, "\n ***** Type \"/bye\" to quit!! ***** \n\n");
    wrefresh(outputScr);

    while(isRunning)
    {
        pthread_mutex_lock(&mutexRecv);

        // wait until buffRecv is valid == is full == not empty
        while(buffRecv.isValid == false)
        {
            pthread_cond_wait(&notEmptyRecv, &mutexRecv);
        }

        // if got a NEW message!! then display the message

        // case : special message like "/bye" or "/hello"
        if(strcmp(buffRecv.msg, "/bye\n") == 0 || strcmp(buffRecv.msg, "/hello") == 0)
        {
            if(strcmp(buffRecv.msg, "/bye\n") == 0)
                wprintw(outputScr, "[%s] exited!!\n", buffRecv.sender);

            else
                wprintw(outputScr, "[%s] entered!!\n", buffRecv.sender);
            
            wrefresh(outputScr);
        }

        // default, normal message
        else
        {
            wprintw(outputScr, "[%s] %d : %s", buffRecv.sender, buffRecv.id, buffRecv.msg);
            wrefresh(outputScr);
        }

        // the buffRecv message is no longer vaild
        buffRecv.isValid = false;

        // UNLOCK!! the mutex and notify the message is not vaild
        pthread_mutex_unlock(&mutexRecv);
        pthread_cond_signal(&notFullRecv);
    }

    return NULL;
}

void* getInputMessageThread()
{
    char temp[BUFF_SIZE];

    while(isRunning)
    {
        pthread_mutex_lock(&mutexSend);

        // wait until buffSend message is send == not valid == not full
        while(buffSend.isValid == true)
        {
            pthread_cond_wait(&notFullSend, &mutexSend);
        }

        // put the input message in temp[]
        mvwgetstr(inputScr, 1, 1, temp);

        // write to buffSend
        sprintf(buffSend.msg, "%s\n", temp);
        buffSend.isValid = true;

        pthread_mutex_unlock(&mutexSend);
        pthread_cond_signal(&notEmptySend);

        // when type "/bye", chatting is end
        if(strcmp(buffSend.msg, "/bye\n") == 0)
        {
            isRunning = 0;
        }

        werase(inputScr);
        box(inputScr, ACS_VLINE, ACS_HLINE);
        wrefresh(inputScr);
        usleep(100);
    }

    // notify to the threads maybe waiting
    pthread_cond_signal(&notEmptySend);

    return NULL;
}

void* writeMessageToShmThread()
{
    while(isRunning)
    {
        pthread_mutex_lock(&mutexSend);

        // only when buffSend is full, write the message
        while(buffSend.isValid == false)
        {
            pthread_cond_wait(&notEmptySend, &mutexSend);
        }

        /* ========== start of the shared memory section ========== */
        while(1)
        {
            sem_wait(sem);

            if(shmPtr->message.isValid == false)
                break;

            sem_post(sem);
            usleep(100);
        }

        // WRITE!! to the shared memory
        memcpy(&(shmPtr->message), &buffSend, sizeof(Message));
        shmPtr->message.id = shmPtr->nextMsgID++;
        shmPtr->message.isValid = true;
        shmPtr->readCount = 0;

        // buffSend message is no longer valid
        buffSend.isValid = false;

        sem_post(sem);
        /* ========== end of the shared memory section ========== */

        pthread_mutex_unlock(&mutexSend);
        pthread_cond_signal(&notFullSend);
    }

    return NULL;
}

void* displayLoginUserThread()
{
    while(isRunning)
    {
        sem_wait(sem);

        // if the number of login users is changed
        // update the information of loginUser
        if(loginUser.numOfUser != shmPtr->loginUser.numOfUser)
        {
            memcpy(&loginUser, &(shmPtr->loginUser), sizeof(LoginUser));
        }

        sem_post(sem);

        // display the information updated
        werase(loginScr);
        mvwprintw(loginScr, 1, 1, "# of users : %d", loginUser.numOfUser);
        for(int i = 0; i < loginUser.numOfUser; i++)
        {
            mvwprintw(loginScr, i+2, 1, "%s", loginUser.user[i]);
        }
        box(loginScr, ACS_VLINE, ACS_HLINE);
        wrefresh(loginScr);
    }

    return NULL;
}

void* displayTimeThread()
{
    time_t     start;
    time_t     now;
    time_t     elapsed;
    struct tm  ts;

    char currentTime[10];
    char elapsedTime[10];

    // record start time
    time(&start);

    while(isRunning)
    {
        // get current time
        time(&now);

        //formatting in "hh-mm-ss"
        ts = *localtime(&now);
        strftime(currentTime, 10, "%H-%M-%S", &ts);

        // ealpsed time = current time - start time
        elapsed = now - start;
        ts = *gmtime(&elapsed);
        strftime(elapsedTime, 10, "%H-%M-%S", &ts);

        mvwprintw(timeScr, 1, 5, "%s", currentTime);
        mvwprintw(timeScr, 2, 5, "%s", elapsedTime);
        wrefresh(timeScr);
        
        // 500,000us == 500ms
        usleep(500000);
    }

    return NULL;
}

// when exit the chatting room
// I have to some actions
// delete windows etc...
void chatExit()
{
    delwin(inputScr);
    delwin(outputScr);
    delwin(loginScr);
    delwin(timeScr);
    delwin(outputBox);
    endwin();

    pthread_mutex_destroy(&mutexSend);
    pthread_cond_destroy(&notFullSend);
    pthread_cond_destroy(&notEmptySend);
    pthread_mutex_destroy(&mutexRecv);
    pthread_cond_destroy(&notFullRecv);
    pthread_cond_destroy(&notEmptyRecv);

    // LOCK!!
    while(1)
    {
        sem_wait(sem);

        if(shmPtr->message.isValid == false)
            break;

        sem_post(sem);
        usleep(100);
    }

    // find my userID on loginUser in the shared memory
    // and remove it
    int idx = 0;
    while(strcmp(shmPtr->loginUser.user[idx], buffRecv.sender) != 0)
    {
        idx++;
    }
    for(; idx < shmPtr->loginUser.numOfUser; idx++)
    {
        strcpy(shmPtr->loginUser.user[idx], shmPtr->loginUser.user[idx+1]);
    }
    memset(shmPtr->loginUser.user[idx], 0, sizeof(char)*ID_SIZE);

    shmPtr->loginUser.numOfUser -= 1;

    // the last user to exit removes the named semaphore and the shared memory
    if(shmPtr->loginUser.numOfUser == 0)
    {
        sem_unlink("sem");

        if(shmctl(shmid, IPC_RMID, NULL) < 0)
        {
            fprintf(stderr, "Failed to delete shared memory!!\n");
            exit(-1);
        }

        printf("Successfully delete shared memory.\n");
    }

    // UNLOCK!!
    else
    {
        sem_post(sem);
    }
    
}

void sigintHandler(int signo)
{
    isRunning = 0;
    chatExit();
    exit(0);
}
