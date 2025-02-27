#include "proxy.h"
#include <ctype.h>

typedef enum {
    STATE_INIT,      // 클라이언트의 초기 요청 대기
    STATE_PROCESS,   // 초기 요청 파싱 및 원격 서버 연결 설정
    STATE_TUNNEL,    // HTTPS CONNECT 요청의 터널링 확립 후
    STATE_RELAY,     // 클라이언트와 원격 서버 간 데이터 중계
    STATE_CLOSING    // 에러 발생 또는 연결 종료 후 정리
} connection_state_t;


// Blocklist를 저장할 전역 변수들
char *blocked_domains[MAX_BLOCKED_DOMAINS];
int blocked_count = 0;

// blocked.txt 파일로부터 차단할 도메인 목록을 로드함
void load_blocked_domains(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        perror("blocked.txt 파일 열기 실패");
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        // 줄바꿈 문자 제거
        line[strcspn(line, "\r\n")] = '\0';
        if (strlen(line) > 0 && blocked_count < MAX_BLOCKED_DOMAINS) {
            blocked_domains[blocked_count] = strdup(line);
            blocked_count++;
        }
    }
    fclose(fp);
}

// 대소문자 구분 없이 차단 목록에 있는지 확인
int is_blocked_domain(const char *host) {
    for (int i = 0; i < blocked_count; i++) {
        if (strcasecmp(host, blocked_domains[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

// 클라이언트에 차단 응답(403 Forbidden) 전송
void send_blocked_response(int client_socket) {
    FILE *fp = fopen("403message.html", "r");
    if (!fp) {
        perror("403message.html 파일 열기 실패");
        return;
    }
    
    // 파일 크기를 구해서 동적 메모리 할당
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    char *blocked_html = malloc(fsize + 1);
    if (!blocked_html) {
        perror("메모리 할당 실패");
        fclose(fp);
        return;
    }
    
    fread(blocked_html, 1, fsize, fp);
    blocked_html[fsize] = '\0';
    fclose(fp);

    // HTTP 응답 헤더 작성
    char response[1024];
    snprintf(response, sizeof(response),
             "HTTP/1.1 403 Forbidden\r\n"
             "Content-Type: text/html\r\n"
             "Content-Length: %ld\r\n"
             "\r\n", fsize);
    
    // 헤더 전송
    if (write(client_socket, response, strlen(response)) < 0) {
        perror("헤더 전송 실패");
        free(blocked_html);
        return;
    }
    // 파일 내용 전송
    if (write(client_socket, blocked_html, fsize) < 0) {
        perror("본문 전송 실패");
    }
    
    free(blocked_html);
}

void handle_client(int client_socket) {
    connection_state_t state = STATE_INIT;
    int remote_socket = -1;
    char buffer[BUFFER_SIZE];
    int n;
    /*int is_https = 0; // 1: HTTPS, 0: HTTP*/

    while (1) {
        switch (state) {
            case STATE_INIT:
                // 클라이언트로부터 초기 요청 대기
                n = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
                if (n <= 0) {
                    state = STATE_CLOSING;
                    break;
                }
                buffer[n] = '\0';
                state = STATE_PROCESS;
                break;

            case STATE_PROCESS: {
                // 요청의 유형(HTTPS CONNECT vs HTTP) 파악 및 원격 서버 연결
                if (strncmp(buffer, "CONNECT", 7) == 0) {
                    //is_https = 1;
                    char target[256];
                    int port = DEFAULT_HTTPS_PORT;
                    char *p = buffer + 8;  // "CONNECT " 이후부터 파싱
                    char *end = strstr(p, " ");
                    if (!end) {
                        state = STATE_CLOSING;
                        break;
                    }
                    *end = '\0';  // host:port 분리

                    char *colon = strchr(p, ':');
                    if (colon) {
                        *colon = '\0';
                        strncpy(target, p, sizeof(target) - 1);
                        target[sizeof(target) - 1] = '\0';
                        port = atoi(colon + 1);
                    } else {
                        strncpy(target, p, sizeof(target) - 1);
                        target[sizeof(target) - 1] = '\0';
                    }
                    printf("HTTPS 요청: %s:%d\n", target, port);

                    // 차단 도메인 체크
                    if (is_blocked_domain(target)) {
                        printf("차단된 도메인: %s\n", target);
                        send_blocked_response(client_socket);
                        state = STATE_CLOSING;
                        break;
                    }
                    // 원격 서버 연결
                    struct hostent *he = gethostbyname(target);
                    if (!he) {
                        perror("gethostbyname");
                        state = STATE_CLOSING;
                        break;
                    }
                    remote_socket = socket(AF_INET, SOCK_STREAM, 0);
                    if (remote_socket < 0) {
                        perror("socket");
                        state = STATE_CLOSING;
                        break;
                    }
                    struct sockaddr_in remote_addr;
                    memset(&remote_addr, 0, sizeof(remote_addr));
                    remote_addr.sin_family = AF_INET;
                    remote_addr.sin_port = htons(port);
                    memcpy(&remote_addr.sin_addr, he->h_addr_list[0], he->h_length);
                    if (connect(remote_socket, (struct sockaddr *)&remote_addr, sizeof(remote_addr)) < 0) {
                        perror("connect");
                        state = STATE_CLOSING;
                        break;
                    }
                    // HTTPS의 경우, 터널링 시작 응답 전송
                    const char *established = "HTTP/1.1 200 Connection Established\r\n\r\n";
                    if (write(client_socket, established, strlen(established)) < 0) {
                        perror("write to client");
                        state = STATE_CLOSING;
                        break;
                    }
                    state = STATE_TUNNEL;
                } else {  // HTTP 요청 처리
                    //is_https = 0;
                    char *host_header = strstr(buffer, "Host:");
                    if (!host_header) {
                        fprintf(stderr, "Host header not found\n");
                        state = STATE_CLOSING;
                        break;
                    }
                    host_header += 5; // "Host:" 건너뜀
                    while (*host_header == ' ' || *host_header == '\t')
                        host_header++;
                    char host[256];
                    int i = 0;
                    while (*host_header != '\r' && *host_header != '\n' &&
                           *host_header != '\0' && i < (int)(sizeof(host) - 1)) {
                        host[i++] = *host_header++;
                    }
                    host[i] = '\0';
                    printf("HTTP 요청: %s\n", host);

                    if (is_blocked_domain(host)) {
                        printf("차단된 도메인: %s\n", host);
                        send_blocked_response(client_socket);
                        state = STATE_CLOSING;
                        break;
                    }
                    int port = DEFAULT_HTTP_PORT;
                    struct hostent *he = gethostbyname(host);
                    if (!he) {
                        perror("gethostbyname");
                        state = STATE_CLOSING;
                        break;
                    }
                    remote_socket = socket(AF_INET, SOCK_STREAM, 0);
                    if (remote_socket < 0) {
                        perror("socket");
                        state = STATE_CLOSING;
                        break;
                    }
                    struct sockaddr_in remote_addr;
                    memset(&remote_addr, 0, sizeof(remote_addr));
                    remote_addr.sin_family = AF_INET;
                    remote_addr.sin_port = htons(port);
                    memcpy(&remote_addr.sin_addr, he->h_addr_list[0], he->h_length);
                    if (connect(remote_socket, (struct sockaddr *)&remote_addr, sizeof(remote_addr)) < 0) {
                        perror("connect");
                        state = STATE_CLOSING;
                        break;
                    }
                    // HTTP 요청을 원격 서버에 전달
                    if (write(remote_socket, buffer, n) < 0) {
                        perror("write to remote");
                        state = STATE_CLOSING;
                        break;
                    }
                    state = STATE_RELAY;
                }
                break;
            }

            case STATE_TUNNEL:
                // HTTPS 터널링의 경우, 터널이 확립된 후 즉시 데이터 중계 상태로 전환
                state = STATE_RELAY;
                break;

            case STATE_RELAY: {
                // 클라이언트와 원격 서버 간의 양방향 데이터 중계
                fd_set read_fds;
                int max_fd = (client_socket > remote_socket) ? client_socket : remote_socket;
                FD_ZERO(&read_fds);
                FD_SET(client_socket, &read_fds);
                FD_SET(remote_socket, &read_fds);
                int activity = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
                if (activity < 0) {
                    perror("select");
                    state = STATE_CLOSING;
                    break;
                }
                if (FD_ISSET(client_socket, &read_fds)) {
                    n = read(client_socket, buffer, BUFFER_SIZE);
                    if (n <= 0) {
                        state = STATE_CLOSING;
                        break;
                    }
                    if (write(remote_socket, buffer, n) < 0) {
                        perror("write to remote");
                        state = STATE_CLOSING;
                        break;
                    }
                }
                if (FD_ISSET(remote_socket, &read_fds)) {
                    n = read(remote_socket, buffer, BUFFER_SIZE);
                    if (n <= 0) {
                        state = STATE_CLOSING;
                        break;
                    }
                    if (write(client_socket, buffer, n) < 0) {
                        perror("write to client");
                        state = STATE_CLOSING;
                        break;
                    }
                }
                break;
            }

            case STATE_CLOSING:
                if (remote_socket >= 0)
                    close(remote_socket);
                return;  // 함수 종료, 스레드 종료
        }
    }
}
