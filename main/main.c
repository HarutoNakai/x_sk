#include <unistd.h> 
#include <stdio.h>
#include <string.h> 
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h" // 【NEW】イベントグループ用
#include "esp_vfs_dev.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "secrets.h"
#include "esp_log.h" // 【NEW】ログレベル制御用


// --- 本格的なファイルシステム用のライブラリ ---
#include "esp_vfs_fat.h"
#include <dirent.h>     
#include <sys/stat.h>

// --- Wi-Fiとネットワーク用のライブラリ ---
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_mac.h"

// --- TCP/IPとソケット通信用のライブラリ ---
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_timer.h"


#define MAX_ARGS 10
//キューの箱
QueueHandle_t mailbox = NULL;

// --- シェル自身に現在地を記憶させる変数 ---
char current_dir[256] = "/storage";


// --- HARUTOS プロセス管理テーブル (xv6ライク) ---
#define MAX_PROCS 10

typedef struct {
    int pid;                 // プロセスID
    TaskHandle_t handle;     // FreeRTOSのタスクハンドル
    volatile int killed;     // ★xv6スタイルの「死の宣告」フラグ (1なら処刑対象)
    int is_active;           // 1なら使用中、0なら空き
} harutos_proc_t;

harutos_proc_t proc_table[MAX_PROCS]; // OSが管理するプロセス名簿

typedef struct {
    const char *name;      
    const char *help_desc; 
    void (*func)(int argc, char *argv[]); 
} command_t;


// --- 【NEW】Wi-Fi接続状態管理用の変数 ---
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
#define MAX_RETRY 3


// --- 【UPDATED】Wi-Fiの状態が変わった時に呼ばれる関数 ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // ここでは何もしない（ユーザーがYESと言ってから接続開始する）
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            printf("\n[Wi-Fi] Retrying connection... (%d/%d)\n", s_retry_num, MAX_RETRY);
        } else {
            // 3回失敗したら「失敗フラグ」を立ててメイン処理に知らせる
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        printf("\n[Wi-Fi] Connected! Got IP Address: " IPSTR "\n", IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        // 接続成功フラグを立てる
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// --- 【UPDATED】Wi-Fiの「初期設定だけ」を行う関数 ---
void wifi_init_core(void) {
    s_wifi_event_group = xEventGroupCreate();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    printf("[Wi-Fi] ESP32-C6 MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n", 
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    // ドライバを起動するだけ（まだ接続はしない）
    ESP_ERROR_CHECK(esp_wifi_start());
}

// --- 【NEW】手動でWi-Fiに接続するコマンド ---
void cmd_wifi_connect(int argc, char *argv[]) {
    // 現在のWi-Fiステータスを取得
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    
    if (bits & WIFI_CONNECTED_BIT) {
        printf("[Wi-Fi] Already connected to '%s'!\n", WIFI_SSID);
        return;
    }

    printf("[Wi-Fi] Manual connection attempt to '%s'...\n", WIFI_SSID);
    s_retry_num = 0; // リトライ回数をリセット
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    
    // 接続処理のキック
    esp_wifi_connect();

    // 成功(CONNECTED)か失敗(FAIL)のフラグが立つまで待機
    bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        // ※IPアドレスの表示自体は裏側のイベントハンドラが行ってくれます
        printf("[Wi-Fi] Manual connection SUCCESS!\n");
    } else if (bits & WIFI_FAIL_BIT) {
        printf("Error: [Wi-Fi] Manual connection FAILED after %d attempts.\n", MAX_RETRY);
    }
}

// --- 【NEW】ユーザーにY/Nを聞く便利関数 ---
bool ask_yes_no(const char* question) {
    printf("%s [Y/n]: ", question);
    fflush(stdout);
    while (1) {
        int c = getchar();
        if (c != EOF) {
            // y か Enter が押されたら YES
            if (c == 'y' || c == 'Y' || c == '\n' || c == '\r') {
                printf("Y\n");
                return true;
            } 
            // n が押されたら NO
            else if (c == 'n' || c == 'N') {
                printf("N\n");
                return false;
            }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// --- 【NEW】対話型のWi-Fi接続シーケンス ---
void interactive_wifi_connect(void) {
    if (!ask_yes_no("\nDo you want to connect to Wi-Fi?")) {
        printf("[Wi-Fi] Skipped. Starting in OFFLINE mode.\n");
        return;
    }

    while (1) {
        printf("[Wi-Fi] Connecting to '%s'...\n", WIFI_SSID);
        s_retry_num = 0;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        
        // 接続開始指示
        esp_wifi_connect();

        // 成功か失敗（3回リトライ後）のどちらかの結果が出るまで待機
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                pdFALSE,
                pdFALSE,
                portMAX_DELAY);

        if (bits & WIFI_CONNECTED_BIT) {
            break; // 接続成功したらループを抜ける
        } else if (bits & WIFI_FAIL_BIT) {
            printf("\n[Wi-Fi] Failed to connect after %d attempts.\n", MAX_RETRY);
            if (!ask_yes_no("Retry connection?")) {
                printf("[Wi-Fi] Giving up. Starting in OFFLINE mode.\n");
                break; // NOなら諦めてループを抜ける
            }
        }
    }
}


// --- 本物のICMPパケット構造体（RFC 792） ---
typedef struct {
    uint8_t type;       
    uint8_t code;       
    uint16_t checksum;  
    uint16_t id;        
    uint16_t sequence;  
    char data[32];      
} __attribute__((packed)) icmp_packet_t;


uint16_t calculate_checksum(uint16_t *addr, int len) {
    int nleft = len;
    uint32_t sum = 0;
    uint16_t *w = addr;
    uint16_t answer = 0;

    while (nleft > 1) {
        sum += *w++;
        nleft -= 2;
    }
    if (nleft == 1) {
        *(uint8_t *)(&answer) = *(uint8_t *)w;
        sum += answer;
    }
    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    answer = ~sum;
    return answer;
}

// --- HTTPクライアントコマンド ---
void cmd_http(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: http <hostname> <path>\n");
        printf("Example: http example.com /\n");
        return;
    }

    const char *hostname = argv[1];
    const char *path = argv[2];

    struct addrinfo hints;
    struct addrinfo *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;       // IPv4
    hints.ai_socktype = SOCK_STREAM; // TCPソケットを指定

    printf("Resolving %s...\n", hostname);
    int err = getaddrinfo(hostname, "80", &hints, &res);
    if (err != 0 || res == NULL) {
        printf("Error: DNS lookup failed for %s\n", hostname);
        return;
    }

    struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
    char ip_str[128];
    inet_ntoa_r(addr->sin_addr, ip_str, sizeof(ip_str) - 1);
    printf("Connecting to %s (%s) port 80...\n", hostname, ip_str);

    int sock = socket(res->ai_family, res->ai_socktype, 0);
    if (sock < 0) {
        printf("Error: Failed to create socket.\n");
        freeaddrinfo(res);
        return;
    }

    struct timeval timeout;
    timeout.tv_sec = 5; 
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        printf("Error: Connection failed.\n");
        close(sock);
        freeaddrinfo(res);
        return;
    }
    freeaddrinfo(res); 

    printf("Connected! Sending HTTP GET request...\n");

    char request[256];
    snprintf(request, sizeof(request), 
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: HARUTOS/0.7 (ESP32-C6)\r\n"
             "Connection: close\r\n\r\n", 
             path, hostname);
    
    if (send(sock, request, strlen(request), 0) < 0) {
        printf("Error: Failed to send request.\n");
        close(sock);
        return;
    }

    char rx_buffer[128];
    int len;
    printf("\n--- HTTP RESPONSE ---\n");
    do {
        len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
        if (len > 0) {
            rx_buffer[len] = '\0'; 
            printf("%s", rx_buffer);
        }
    } while (len > 0);
    printf("\n---------------------\n");

    close(sock);
}

// --- ガチのPingコマンド ---
void cmd_ping(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: ping <IP Address>\n");
        return;
    }

    const char *target_ip = argv[1];
    printf("PING %s: 32 data bytes\n", target_ip);

    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        printf("Error: Failed to create raw socket.\n");
        return;
    }

    struct timeval timeout;
    timeout.tv_sec = 3; 
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in dest_addr;
    dest_addr.sin_family = AF_INET;
    inet_pton(AF_INET, target_ip, &dest_addr.sin_addr);

    icmp_packet_t icmp_req;
    memset(&icmp_req, 0, sizeof(icmp_req)); 
    icmp_req.type = 8;         
    icmp_req.code = 0;
    icmp_req.id = htons(1234); 
    icmp_req.sequence = htons(1); 
    
    strcpy(icmp_req.data, "HARUTOS PING TEST PACKET!!!!!!");
    
    icmp_req.checksum = calculate_checksum((uint16_t *)&icmp_req, sizeof(icmp_req));

    int64_t start_time = esp_timer_get_time();
    int err = sendto(sock, &icmp_req, sizeof(icmp_req), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        printf("Error: Failed to send packet.\n");
        close(sock);
        return;
    }

    char rx_buffer[128];
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);
    
    int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&source_addr, &socklen);
    int64_t end_time = esp_timer_get_time();

    if (len > 0) {
        icmp_packet_t *icmp_reply = (icmp_packet_t *)(rx_buffer + 20);
        
        if (icmp_reply->type == 0) { 
            printf("%d bytes from %s: icmp_seq=%d time=%lld ms\n", 
                   len - 20, target_ip, ntohs(icmp_reply->sequence), (end_time - start_time) / 1000);
        } else {
            printf("Received ICMP Type %d\n", icmp_reply->type);
        }
    } else {
        printf("Request timeout for icmp_seq 1\n");
    }

    close(sock);
}

void cmd_clear(int argc, char *argv[]) { printf("\033[2J\033[H"); }
void cmd_fuck(int argc, char *argv[]) { printf("FUCK YOU\n"); }
void cmd_neko(int argc, char *argv[]) {
    printf(" /\\_/\\\n( o.o )\n > ^ <\n");
}
void cmd_echo(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) printf("%s ", argv[i]);
    printf("\n");
}

void cmd_nakai(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) printf("%s ", argv[i]);
    printf("\n");
}

void cmd_pwd(int argc, char *argv[]) {
    printf("%s\n", current_dir);
}

void cmd_cd(int argc, char *argv[]) {
    if (argc < 2) {
        strcpy(current_dir, "/storage"); 
        return;
    }
    if (strcmp(argv[1], "..") == 0) {
        char *last_slash = strrchr(current_dir, '/');
        if (last_slash != NULL && strcmp(current_dir, "/storage") != 0) {
            *last_slash = '\0';
        }
        return;
    }
    
    char path[300];
    snprintf(path, sizeof(path), "%s/%s", current_dir, argv[1]);
    DIR *dir = opendir(path);
    if (dir) {
        closedir(dir);
        strcpy(current_dir, path); 
    } else {
        printf("Error: No such directory '%s'\n", argv[1]);
    }
}

void cmd_ls(int argc, char *argv[]) {
    char target_dir[300];
    if (argc > 1) {
        snprintf(target_dir, sizeof(target_dir), "%s/%s", current_dir, argv[1]);
    } else {
        strcpy(target_dir, current_dir);
    }
    
    DIR *dir = opendir(target_dir);
    if (dir == NULL) {
        printf("Error: Failed to open directory\n");
        return;
    }

    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        struct stat entry_stat;
        char path[600];
        snprintf(path, sizeof(path), "%s/%s", target_dir, entry->d_name);
        stat(path, &entry_stat);

        if (S_ISDIR(entry_stat.st_mode)) {
            printf("  %-16s <DIR>\n", entry->d_name);
        } else {
            printf("  %-16s %7ld bytes\n", entry->d_name, entry_stat.st_size);
        }
        count++;
    }
    closedir(dir);
    if (count == 0) printf("  (empty)\n");
}

void cmd_mkdir(int argc, char *argv[]) {
    if (argc < 2) return;
    char path[300];
    snprintf(path, sizeof(path), "%s/%s", current_dir, argv[1]);
    if (mkdir(path, 0777) == 0) printf("Directory '%s' created.\n", argv[1]);
    else printf("Error: Failed to create directory\n");
}

// --- バックグラウンドプロセス（受信側 / xv6自決対応） ---
void background_task(void *pvParameters) {
    int p_index = (int)pvParameters; 
    int pid = proc_table[p_index].pid;
    char rx_msg[128]; 

    printf("\n[Process %d] Started. Waiting for messages...\n", pid);
    printf("HARUTOS:%s$ ", current_dir);
    fflush(stdout);

    while (1) {
        if (proc_table[p_index].killed) {
            printf("\n[Process %d] Killed flag detected. Cleaning up and exiting gracefully...\n", pid);
            proc_table[p_index].is_active = 0; 
            printf("HARUTOS:%s$ ", current_dir);
            fflush(stdout);
            vTaskDelete(NULL); 
        }

        if (xQueueReceive(mailbox, &rx_msg, portMAX_DELAY) == pdTRUE) {
            printf("\n[Process %d] Received message: '%s'\n", pid, rx_msg);
            printf("HARUTOS:%s$ ", current_dir);
            fflush(stdout);
            
            if (strcmp(rx_msg, "exit") == 0) {
                printf("\n[Process %d] Exit command received. Terminating...\n", pid);
                proc_table[p_index].is_active = 0;
                printf("HARUTOS:%s$ ", current_dir);
                fflush(stdout);
                vTaskDelete(NULL);
            }
        }
    }
}

// --- プロセス生成（プロセステーブルへの登録機能追加） ---
void cmd_spawn(int argc, char *argv[]) {
    static int next_pid = 100;
    
    if (mailbox == NULL) {
        mailbox = xQueueCreate(5, 128);
    }

    int slot = -1;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].is_active == 0) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        printf("Error: Max processes reached!\n");
        return;
    }

    proc_table[slot].pid = next_pid;
    proc_table[slot].killed = 0;
    proc_table[slot].is_active = 1;

    printf("Spawning background process (PID: %d)...\n", next_pid);
    xTaskCreate(background_task, "bg_task", 4096, (void *)slot, 1, &proc_table[slot].handle);
    next_pid++;
}

// --- xv6スタイルの Kill コマンド ---
void cmd_kill(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: kill <PID>\n");
        return;
    }

    int target_pid = atoi(argv[1]);

    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].is_active && proc_table[i].pid == target_pid) {
            
            proc_table[i].killed = 1;
            printf("Kill signal sent to Process %d.\n", target_pid);

            xTaskAbortDelay(proc_table[i].handle);
            
            return;
        }
    }
    printf("Error: PID %d not found.\n", target_pid);
}

// --- プロセス間通信の送信コマンド ---
void cmd_send(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: send <message>\n");
        return;
    }
    if (mailbox == NULL) {
        printf("Error: No mailbox exists. Spawn a process first!\n");
        return;
    }

    char tx_msg[128] = "";
    for (int i = 1; i < argc; i++) {
        strncat(tx_msg, argv[i], sizeof(tx_msg) - strlen(tx_msg) - 1);
        if (i < argc - 1) strncat(tx_msg, " ", sizeof(tx_msg) - strlen(tx_msg) - 1);
    }

    if (xQueueSend(mailbox, &tx_msg, 0) == pdPASS) {
        printf("Message pushed to queue.\n");
    } else {
        printf("Error: Queue is full!\n");
    }
}


// --- IPC（プロセス間通信）の監視コマンド ---
void cmd_ipcs(int argc, char *argv[]) {
    if (mailbox == NULL) {
        printf("IPC Mailbox is not created yet.\n");
        return;
    }

    UBaseType_t waiting = uxQueueMessagesWaiting(mailbox);
    UBaseType_t available = uxQueueSpacesAvailable(mailbox);

    printf("------ IPC Mailbox Status ------\n");
    printf(" Messages waiting : %u\n", (unsigned int)waiting);
    printf(" Spaces available : %u\n", (unsigned int)available);

    if (waiting > 0) {
        char peek_msg[128];
        if (xQueuePeek(mailbox, &peek_msg, 0) == pdTRUE) {
            printf(" Next message     : '%s'\n", peek_msg);
        }
    }
    printf("--------------------------------\n");
}


// --- プロセス一覧表示コマンド ---
void cmd_ps(int argc, char *argv[]) {
    printf("%-16s %-7s %-7s %-7s\n", "NAME", "STATE", "PRIO", "STACK");
    printf("--------------------------------------------\n");
    
    volatile UBaseType_t uxArraySize = uxTaskGetNumberOfTasks();
    TaskStatus_t *pxTaskStatusArray = malloc(uxArraySize * sizeof(TaskStatus_t));
    
    if (pxTaskStatusArray != NULL) {
        uxArraySize = uxTaskGetSystemState(pxTaskStatusArray, uxArraySize, NULL);
        
        for (UBaseType_t i = 0; i < uxArraySize; i++) {
            char state = '?';
            switch (pxTaskStatusArray[i].eCurrentState) {
                case eRunning:   state = 'R'; break; 
                case eReady:     state = 'r'; break; 
                case eBlocked:   state = 'B'; break; 
                case eSuspended: state = 'S'; break; 
                case eDeleted:   state = 'D'; break; 
                default:         state = '?'; break;
            }

            printf("%-16s %-7c %-7u %-7u\n",
                   pxTaskStatusArray[i].pcTaskName,
                   state,
                   (unsigned int)pxTaskStatusArray[i].uxCurrentPriority,
                   (unsigned int)pxTaskStatusArray[i].usStackHighWaterMark);
        }
        free(pxTaskStatusArray);
    } else {
        printf("Error: Memory allocation failed for PS command.\n");
    }
}


void cmd_write(int argc, char *argv[]) {
    if (argc < 3) return;
    char path[300];
    snprintf(path, sizeof(path), "%s/%s", current_dir, argv[1]);
    FILE *f = fopen(path, "w");
    if (f) {
        for (int i = 2; i < argc; i++) fprintf(f, "%s ", argv[i]);
        fprintf(f, "\n");
        fclose(f);
        printf("File '%s' written.\n", argv[1]);
    } else {
        printf("Error: Failed to open file\n");
    }
}

void cmd_cat(int argc, char *argv[]) {
    if (argc < 2) return;
    char path[300];
    snprintf(path, sizeof(path), "%s/%s", current_dir, argv[1]);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f) != NULL) printf("%s", line);
        fclose(f);
    } else {
        printf("Error: File not found\n");
    }
}

void cmd_rm(int argc, char *argv[]) {
    if (argc < 2) return;
    char path[300];
    snprintf(path, sizeof(path), "%s/%s", current_dir, argv[1]);
    if (remove(path) == 0) printf("Removed '%s'\n", argv[1]);
    else printf("Error: Failed to remove '%s'\n", argv[1]);
}
void cmd_help(int argc, char *argv[]); 

command_t commands[] = {
    {"help",  "Show available commands", cmd_help},
    {"ping",  "Test connectivity",       cmd_ping},
    {"clear", "Clear the screen",        cmd_clear},
    {"fuck",  "Special greeting",        cmd_fuck},
    {"neko",  "show NEKO",               cmd_neko},
    {"echo",  "Print arguments",         cmd_echo},
    {"ls",    "List directory contents", cmd_ls},
    {"write", "Write text to a file",    cmd_write}, 
    {"cat",   "Read a file",             cmd_cat},   
    {"rm",    "Remove a file",           cmd_rm},
    {"mkdir", "Make directory",          cmd_mkdir},
    {"cd",    "Change directory",        cmd_cd},
    {"pwd",   "Print working dir",       cmd_pwd},
    {"spawn", "Spawn background task",   cmd_spawn}, 
    {"ps",    "Show process status",     cmd_ps},
    {"send",  "Send IPC message",        cmd_send},
    {"ipcs",  "Show IPC status",         cmd_ipcs},
    {"kill",  "Kill a process",          cmd_kill},
    {"http",  "Send HTTP GET request",   cmd_http},
    {"wifi_connect", "Connect to Wi-Fi", cmd_wifi_connect}
};

#define NUM_COMMANDS (sizeof(commands) / sizeof(command_t))

void cmd_help(int argc, char *argv[]) {
    printf("Available commands:\n");
    for (int i = 0; i < NUM_COMMANDS; i++) {
        printf("  %-8s : %s\n", commands[i].name, commands[i].help_desc);
    }
}


void app_main(void)
{
    // 【NEW】ESP-IDFのシステムログ（I や W）をすべて黙らせる（エラーのみ許可）
    esp_log_level_set("*", ESP_LOG_ERROR);
    usb_serial_jtag_driver_config_t usb_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&usb_config);
    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("\n[HARUTOS] Mounting FATFS (Real File System)...\n");
    const esp_vfs_fat_mount_config_t mount_config = {
        .max_files = 5,
        .format_if_mount_failed = true, 
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE
    };
    wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl("/storage", "storage", &mount_config, &s_wl_handle);
    
    if (err != ESP_OK) {
        printf("[HARUTOS] Failed to mount FATFS!\n");
    } else {
        printf("[HARUTOS] FATFS Mounted perfectly.\n");
    }

    char rx_buffer[64]; 
    int rx_pos = 0;     

    printf("\n");
    printf("  _    _          _____  _    _ _______ ____   _____ \n");
    printf(" | |  | |   /\\   |  __ \\| |  | |__   __/ __ \\ / ____|\n");
    printf(" | |__| |  /  \\  | |__) | |  | |  | | | |  | | (___  \n");
    printf(" |  __  | / /\\ \\ |  _  /| |  | |  | | | |  | |\\___ \\ \n");
    printf(" | |  | |/ ____ \\| | \\ \\| |__| |  | | | |__| |____) |\n");
    printf(" |_|  |_/_/    \\_\\_|  \\_\\\\____/   |_|  \\____/|_____/ \n");
    printf("\n");
    printf("--- Welcome to HARUTOS v0.7 (Microkernel with IPC) ---\n");
    
    // 【NEW】ブート時のWi-Fi対話処理
    wifi_init_core();
    interactive_wifi_connect();

    printf("\nHARUTOS:%s$ ", current_dir);
    fflush(stdout);

    while (1) {
        int c = getchar();

        if (c != EOF) {
            if (c == '\n' || c == '\r') {
                printf("\n"); 
                rx_buffer[rx_pos] = '\0'; 

                if (strlen(rx_buffer) > 0) {
                    char *argv[MAX_ARGS]; 
                    int argc = 0;      
                    
                    char *token = strtok(rx_buffer, " ");
                    while (token != NULL && argc < MAX_ARGS) {
                        argv[argc] = token; 
                        argc++;
                        token = strtok(NULL, " "); 
                    }

                    if (argc > 0) {
                        int found = 0;
                        for (int i = 0; i < NUM_COMMANDS; i++) {
                            if (strcmp(argv[0], commands[i].name) == 0) {
                                commands[i].func(argc, argv); 
                                found = 1;
                                break;
                            }
                        }
                        if (!found) printf("Command not found: %s\n", argv[0]);
                    }
                }

                rx_pos = 0;
                printf("HARUTOS:%s$ ", current_dir);
                fflush(stdout);
            } 
            else if (c == '\b' || c == 127) {
                if (rx_pos > 0) {
                    rx_pos--; 
                    printf("\b \b"); 
                    fflush(stdout);
                }
            }
            else {
                if (rx_pos < sizeof(rx_buffer) - 1) {
                    putchar(c); 
                    fflush(stdout);
                    rx_buffer[rx_pos++] = c;
                }
            }
        }
        vTaskDelay(1);
    }
}