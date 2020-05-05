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

#define ID_SIZE 16      // the max length of user ID
#define BUFF_SIZE 1024  // the max length a single message
#define QUEUE_SIZE 10   // the size of message queue in shared memory
#define MAX_USER 18     // the number of users can join chatting room

// struct of a single message
typedef struct
{
    int  id;
    char sender[ID_SIZE];
    char msg[BUFF_SIZE];
} Message;

// struct for shared memory
typedef struct
{
    Message message;
    int     readCount;
    int     nextMsgID;
    int     numOfUser;
    sem_t*  sem;
} ShmStruct;

int        shmid;
ShmStruct* shmPtr;  // *shmPtr points to the shared memory segment
sem_t*     sem;     // semaphore to synchronize the shared memory among processes

WINDOW* outputScr;
WINDOW* inputScr;
WINDOW* loginScr;
WINDOW* timeScr;

Message buffSend;  // message to send
Message buffRecv;  // message to receive

pthread_mutex_t mutexSend;
pthread_cond_t notFullSend;
pthread_cond_t notEmptySend;

pthread_mutex_t mutexRecv;
pthread_cond_t notFullRecv;
pthread_cond_t notEmptyRecv;

int isRunning;

void chatInit();
void chat();
void cleanup();

void* fetchMessageFromShmThread();
void* writeMessageToShmThread();
void* displayMessageThread();
void* getInputMessageThread();

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
    chat();
    cleanup();

    return 0;
}

void chatInit()
{
    // get the shmid of shared memory
    shmid = shmget((key_t)201624416, sizeof(ShmStruct), 0644);

    // if this process is the FIRST
    // create the shared memory and attach it,
    // initialize common variables and create the semaphore
    if(shmid < 0)
    {
        shmid = shmget((key_t)201624416, sizeof(ShmStruct), 0644|IPC_CREAT|IPC_EXCL);
        shmPtr = (ShmStruct*)shmat(shmid, NULL, 0644);

        shmPtr->readCount = 0;
        shmPtr->nextMsgID = 1;
        shmPtr->numOfUser = 0;

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

    // before join chatting room
    // check that can I enter
    if(shmPtr->numOfUser == MAX_USER)
    {
        fprintf(stderr, "chatting room is FULL!!\n");
        exit(-1);
    }

    // LOCK!!
    sem_wait(sem);
    // and ENTER!
    shmPtr->numOfUser++;
    // UNLOCK!!
    sem_post(sem);


    // initialize mutex
    pthread_mutex_init(&mutexSend, NULL);
    pthread_cond_init(&notFullSend, NULL);
    pthread_cond_init(&notEmptySend, NULL);

    pthread_mutex_init(&mutexRecv, NULL);
    pthread_cond_init(&notFullRecv, NULL);
    pthread_cond_init(&notEmptyRecv, NULL);

    // start ncurses mode
    initscr();

    // make 4 subwindows
    outputScr = subwin(stdscr, 20, 60,  0,  0);
    inputScr  = subwin(stdscr,  4, 60, 20,  0);
    loginScr  = subwin(stdscr, 20, 20,  0, 60);
    timeScr   = subwin(stdscr,  4, 20, 20, 60);

    // draw boxline of each window
    box(outputScr, ACS_VLINE, ACS_HLINE);
    box(inputScr, ACS_VLINE, ACS_HLINE);
    box(loginScr, ACS_VLINE, ACS_HLINE);
    box(timeScr, ACS_VLINE, ACS_HLINE);

    wrefresh(outputScr);
    wrefresh(inputScr);
    wrefresh(loginScr);
    wrefresh(timeScr);
}

// chatting is actually processed in this part
void chat()
{
    buffSend.id = 0;
    buffRecv.id = 0;
    isRunning = 1;


    // create threads and wait for them
    pthread_t thread[4];

    pthread_create(&thread[0], NULL, fetchMessageFromShmThread, NULL);
    pthread_create(&thread[1], NULL, writeMessageToShmThread, NULL);
    pthread_create(&thread[2], NULL, displayMessageThread, NULL);
    pthread_create(&thread[3], NULL, getInputMessageThread, NULL);

    for(int i = 0; i < 4; i++)
    {
        pthread_join(thread[i], NULL);
    }
}

void* fetchMessageFromShmThread()
{
    int lastFetchID = buffRecv.id;

    while(isRunning)
    {
        sem_wait(sem);
        if(lastFetchID != shmPtr->message.id)
        {
            memcpy(&buffRecv, &(shmPtr->message), sizeof(Message));
            shmPtr->readCount++;

            // if all of users read the message
            // reset the read count
            if(shmPtr->readCount >= shmPtr->numOfUser)
            {
                //memset(&(shmPtr->message), 0, sizeof(Message));
                shmPtr->readCount = 0;
            }

            lastFetchID = buffRecv.id;
        }
        sem_post(sem);
    }

    return NULL;
}

void* writeMessageToShmThread()
{
    int lastWriteID = 0;

    while(isRunning)
    {
        sem_wait(sem);
        if(lastWriteID != buffSend.id && shmPtr->readCount == 0)
        {
            memcpy(&(shmPtr->message), &buffSend, sizeof(Message));
            shmPtr->message.id = shmPtr->nextMsgID++;
            lastWriteID = buffSend.id;
        }
        sem_post(sem);
    }

    return NULL;
}

void* displayMessageThread()
{
    WINDOW* subOutputScr = subwin(outputScr, 18, 58, 1, 1);
    scrollok(subOutputScr, TRUE);

    wprintw(subOutputScr, "\n ***** Type \"/bye\" to quit!! ***** \n\n");
    wrefresh(subOutputScr);

    int lastDisplayID = 0;

    while(isRunning)
    {

        if(lastDisplayID != buffRecv.id)
        {
            wprintw(subOutputScr, "[%s] %d : %s", buffRecv.sender, buffRecv.id, buffRecv.msg);
            lastDisplayID = buffRecv.id;
            wrefresh(subOutputScr);
        }
    }

    delwin(subOutputScr);

    return NULL;
}

void* getInputMessageThread()
{
    char temp[BUFF_SIZE];

    while(isRunning)
    {
        // put the input message in temp[]
        mvwgetstr(inputScr, 1, 1, temp);

        // update buffSend
        sprintf(buffSend.msg, "%s\n", temp);
        buffSend.id++;

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

    return NULL;
}

// when exit the chatting room
// I have to some actions
// delete windows etc...
void cleanup()
{
    delwin(inputScr);
    delwin(outputScr);
    delwin(loginScr);
    delwin(timeScr);
    endwin();

    pthread_mutex_destroy(&mutexSend);
    pthread_cond_destroy(&notFullSend);
    pthread_cond_destroy(&notEmptySend);

    pthread_mutex_destroy(&mutexRecv);
    pthread_cond_destroy(&notFullRecv);
    pthread_cond_destroy(&notEmptyRecv);

    sem_wait(sem);

    shmPtr->numOfUser--;

    // the last user to exit removes the shared memory
    if(shmPtr->numOfUser == 0)
    {
        sem_unlink("sem");

        if(shmctl(shmid, IPC_RMID, NULL) < 0)
        {
            fprintf(stderr, "Failed to delete shared memory!!\n");
            exit(-1);
        }

        printf("Successfully delete shared memory.\n");
    }

    else
    {
        sem_post(sem);
    }
    
}
