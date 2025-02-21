#include "proxy.h"
#include <pthread.h>
#include <semaphore.h>

#define MAX_THREADS 200

// 세마포어 전역 변수: 동시에 실행 가능한 최대 스레드 수를 제어
sem_t thread_sem;

void *thread_func(void *arg) {
    int client_socket = *((int *)arg);
    free(arg);
    handle_client(client_socket);
    close(client_socket);
    // 스레드 종료 시 세마포어 슬롯 반환
    sem_post(&thread_sem);
    return NULL;
}

int main(int argc, char *argv[]) {
    int listen_socket, *client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int server_port = DEFAULT_SERVER_PORT;
    pthread_t thread_id;

    // 차단할 도메인 목록 로드
    load_blocked_domains("blocked.txt");

    // 명령행 인자로 포트 번호 지정 시 사용
    if (argc > 1) {
        server_port = atoi(argv[1]);
        if (server_port <= 0) {
            fprintf(stderr, "유효하지 않은 포트 번호: %s\n", argv[1]);
            exit(EXIT_FAILURE);
        }
    }

    listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(listen_socket);
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);

    if (bind(listen_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(listen_socket);
        exit(EXIT_FAILURE);
    }

    if (listen(listen_socket, LISTEN_BACKLOG) < 0) {
        perror("listen");
        close(listen_socket);
        exit(EXIT_FAILURE);
    }

    printf("프록시 서버가 포트 %d에서 대기 중...\n", server_port);

    // 세마포어 초기화: 동시에 MAX_THREADS개의 스레드 슬롯 확보
    sem_init(&thread_sem, 0, MAX_THREADS);

    while (1) {
        client_socket = malloc(sizeof(int));
        *client_socket = accept(listen_socket, (struct sockaddr*)&client_addr, &client_addr_len);
        if (*client_socket < 0) {
            perror("accept");
            free(client_socket);
            continue;
        }

        // 사용 가능한 스레드 슬롯이 생길 때까지 대기
        sem_wait(&thread_sem);

        if (pthread_create(&thread_id, NULL, thread_func, client_socket) != 0) {
            perror("pthread_create");
            free(client_socket);
            sem_post(&thread_sem); // 스레드 생성 실패 시 슬롯 반환
            continue;
        }

        // 스레드가 종료되면 자동으로 자원 반환
        pthread_detach(thread_id);
    }

    close(listen_socket);
    sem_destroy(&thread_sem);
    return 0;
}

