#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <libmseed.h>
#include <time.h>
#include <sys/time.h>

#define MAX_CLIENTS 32
#define SL_RECSIZE 512
#define SL_HDRSIZE 8

typedef struct {
    int enabled;
    char sl_host[128];
    int sl_port;
    char network[8];
    char station[16];
    char channel[8];
    int zejf_port;
} Config;

Config cfg;

static int json_extract_string(const char *json, const char *key, char *out, size_t outsz) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return -1;
    p = strchr(p, '"');
    if (!p) return -1;
    p++;
    const char *q = strchr(p, '"');
    if (!q) return -1;
    size_t n = q - p;
    if (n >= outsz) n = outsz - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

static int json_extract_number(const char *json, const char *key, int *out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return -1;
    p++;
    *out = atoi(p);
    return 0;
}

static int load_config(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *json = malloc(sz + 1);
    size_t read_bytes = fread(json, 1, sz, f);
    (void)read_bytes;
    fclose(f);
    json[sz] = 0;
    
    cfg.enabled = 1;
    strcpy(cfg.sl_host, "seedlink.eqcitizen.org");
    cfg.sl_port = 18000;
    strcpy(cfg.network, "XX");
    strcpy(cfg.station, "EQ002");
    strcpy(cfg.channel, "HHZ");
    cfg.zejf_port = 6222;

    const char *blk = strstr(json, "\"seedlink_to_zejf\"");
    if (blk) {
        json_extract_string(blk, "seedlink_host", cfg.sl_host, sizeof(cfg.sl_host));
        json_extract_number(blk, "seedlink_port", &cfg.sl_port);
        json_extract_string(blk, "seedlink_network", cfg.network, sizeof(cfg.network));
        json_extract_string(blk, "seedlink_station", cfg.station, sizeof(cfg.station));
        json_extract_string(blk, "seedlink_channel", cfg.channel, sizeof(cfg.channel));
        json_extract_number(blk, "zejf_server_port", &cfg.zejf_port);
    }
    free(json);
    return 0;
}

int zejf_clients[MAX_CLIENTS];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

uint64_t current_log_id = 0;
int sample_rate = 100;

#define LOG_ID_FILE "../data/last_log_id.txt"

/* Carga el último log_id guardado en disco. Si no existe o es invalido,
 * cae al valor basado en el reloj (comportamiento original). */
static uint64_t load_last_log_id(void) {
    FILE *f = fopen(LOG_ID_FILE, "r");
    if (f) {
        unsigned long long val = 0;
        if (fscanf(f, "%llu", &val) == 1) {
            fclose(f);
            /* IMPORTANTE: current_log_id debe representar tiempo real
             * (reloj), no "numero de muestras procesadas". Si simplemente
             * devolviesemos el valor persistido, el handshake enviaria un
             * last_log_id correspondiente al momento del ultimo guardado,
             * no a "ahora" -- y ZejfSeis ancla su reloj con ese desfase,
             * mostrando los datos retrasados indefinidamente.
             *
             * Por eso usamos el mayor de:
             *  - el valor basado en el reloj actual (caso normal: no hay
             *    desfase, igual que el comportamiento original)
             *  - el valor persistido + 1 (solo para garantizar que nunca
             *    retrocede, por si el reloj del sistema fuese menor)
             */
            struct timeval tv_now;
            gettimeofday(&tv_now, NULL);
            uint64_t clock_based = (uint64_t)(tv_now.tv_sec * 100 + (tv_now.tv_usec / 10000));
            uint64_t persisted_plus = (uint64_t)val + 1;
            return (clock_based > persisted_plus) ? clock_based : persisted_plus;
        }
        fclose(f);
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)(tv.tv_sec * 100 + (tv.tv_usec / 10000));
}

/* Guarda el log_id actual en disco para que sobreviva a un reinicio.
 * Escritura atómica via fichero temporal + rename. */
static void save_last_log_id(uint64_t id) {
    char tmp[] = LOG_ID_FILE ".tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%llu\n", (unsigned long long)id);
    fclose(f);
    rename(tmp, LOG_ID_FILE);
}

/* Hilo que persiste current_log_id periódicamente, para que si el proceso
 * se reinicia, el handshake "last_log_id" continue donde lo dejó y
 * ZejfSeis no detecte una discontinuidad que le impida reconectar. */
void *log_id_persist_thread(void *arg) {
    uint64_t last_saved = 0;
    while (1) {
        sleep(5);
        uint64_t id = current_log_id;
        if (id != last_saved) {
            save_last_log_id(id);
            last_saved = id;
        }
    }
    return NULL;
}

int mseed_blocks_received = 0;
time_t last_print_time = 0;

void process_mseed(char *record) {
    MS3Record *msr = NULL;
    int ret = msr3_parse(record, SL_RECSIZE, &msr, MSF_UNPACKDATA, 0);
    if (ret == MS_NOERROR) {
        if (msr->numsamples > 0 && msr->datasamples != NULL) {
            sample_rate = (int)msr->samprate;

            /* DIAGNOSTICO: comparar el tiempo de la muestra (segun el
             * propio MiniSEED) con el reloj del sistema, para saber si
             * el retraso viene del feed Seedlink (latencia real del dato)
             * o de un desajuste local en current_log_id / handshake. */
            {
                nstime_t record_start = msr->starttime;
                nstime_t record_end = record_start +
                    (nstime_t)((msr->numsamples - 1) * (NSTMODULUS / msr->samprate));
                time_t record_end_sec = (time_t)(record_end / NSTMODULUS);
                time_t now_sec = time(NULL);
                double delay_sec = (double)(now_sec - record_end_sec);
                static time_t last_delay_print = 0;
                if (now_sec - last_delay_print >= 5) {
                    char timestr[64];
                    ms_nstime2timestr_n(record_end, timestr, sizeof(timestr), ISOMONTHDAY, NANO_MICRO);
                    printf("[Seedlink Bridge] DIAG: last sample time=%s (record end), now=%ld, delay=%.2fs\n",
                           timestr, (long)now_sec, delay_sec);
                    last_delay_print = now_sec;
                }
            }

            /* Resincroniza current_log_id con el reloj real antes de
             * asignar ids a las muestras de este bloque. Esto evita que
             * el contador "se quede atras" respecto al tiempo real por
             * pequeñas pausas/jitter en la llegada de bloques, lo cual
             * causaria un desfase creciente en la grafica de ZejfSeis.
             * Solo avanza hacia delante (nunca retrocede), preservando
             * la monotonia que ZejfSeis necesita. */
            {
                struct timeval tv_now;
                gettimeofday(&tv_now, NULL);
                uint64_t clock_based = (uint64_t)(tv_now.tv_sec * 100 + (tv_now.tv_usec / 10000));
                if (clock_based > current_log_id) {
                    current_log_id = clock_based;
                }
            }

            size_t buf_size = 32 + msr->numsamples * 48;
            char *chunk = malloc(buf_size);
            if (chunk) {
                int offset = 0;
                offset += snprintf(chunk + offset, buf_size - offset, "realtime\n");
                
                if (msr->sampletype == 'i') {
                    int32_t *samples = (int32_t*)msr->datasamples;
                    for (int i=0; i<msr->numsamples; i++) {
                        current_log_id++;
                        offset += snprintf(chunk + offset, buf_size - offset, "%d\n%llu\n", samples[i], (unsigned long long)current_log_id);
                    }
                } else if (msr->sampletype == 'f') {
                    float *samples = (float*)msr->datasamples;
                    for (int i=0; i<msr->numsamples; i++) {
                        current_log_id++;
                        offset += snprintf(chunk + offset, buf_size - offset, "%d\n%llu\n", (int32_t)samples[i], (unsigned long long)current_log_id);
                    }
                } else if (msr->sampletype == 'd') {
                    double *samples = (double*)msr->datasamples;
                    for (int i=0; i<msr->numsamples; i++) {
                        current_log_id++;
                        offset += snprintf(chunk + offset, buf_size - offset, "%d\n%llu\n", (int32_t)samples[i], (unsigned long long)current_log_id);
                    }
                }
                
                offset += snprintf(chunk + offset, buf_size - offset, "-2147483648\n");
                
                pthread_mutex_lock(&clients_mutex);
                for (int i=0; i<MAX_CLIENTS; i++) {
                    if (zejf_clients[i] != -1) {
                        if (send(zejf_clients[i], chunk, offset, MSG_NOSIGNAL) <= 0) {
                            close(zejf_clients[i]);
                            zejf_clients[i] = -1;
                        }
                    }
                }
                pthread_mutex_unlock(&clients_mutex);
                free(chunk);
                
                mseed_blocks_received++;
                time_t now = time(NULL);
                if (now - last_print_time >= 5) { // Print every 5 seconds if receiving data
                    printf("[Seedlink Bridge] Receiving data from %s_%s... (%d blocks processed, %d samples/block)\n", cfg.network, cfg.station, mseed_blocks_received, (int)msr->numsamples);
                    last_print_time = now;
                }
            }
        }
        msr3_free(&msr);
    }
}

void* seedlink_client_thread(void *arg) {
    while (1) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        struct hostent *server = gethostbyname(cfg.sl_host);
        if (!server) { sleep(5); continue; }
        struct sockaddr_in serv_addr;
        memset((char *)&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        memcpy((char *)&serv_addr.sin_addr.s_addr, (char *)server->h_addr, server->h_length);
        serv_addr.sin_port = htons(cfg.sl_port);
        
        printf("[Seedlink Bridge] Connecting to remote Seedlink server %s:%d...\n", cfg.sl_host, cfg.sl_port);
        if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
            printf("[Seedlink Bridge] Error connecting to Seedlink: %s\n", strerror(errno));
            close(sock);
            sleep(5);
            continue;
        }
        printf("[Seedlink Bridge] Connected to %s:%d.\n", cfg.sl_host, cfg.sl_port);
        
        char cmd[256];
        char buf[1024];
        
        snprintf(cmd, sizeof(cmd), "HELLO\r\n");
        send(sock, cmd, strlen(cmd), 0);
        int nr = recv(sock, buf, sizeof(buf)-1, 0);
        if(nr > 0) { buf[nr]=0; printf("[Seedlink Bridge] Reply HELLO: %s\n", buf); }
        
        snprintf(cmd, sizeof(cmd), "STATION %s %s\r\n", cfg.station, cfg.network);
        printf("[Seedlink Bridge] Sending: %s", cmd);
        send(sock, cmd, strlen(cmd), 0);
        nr = recv(sock, buf, sizeof(buf)-1, 0);
        if(nr > 0) { buf[nr]=0; printf("[Seedlink Bridge] Reply STATION: %s\n", buf); }

        snprintf(cmd, sizeof(cmd), "SELECT %s\r\n", cfg.channel);
        printf("[Seedlink Bridge] Sending: %s", cmd);
        send(sock, cmd, strlen(cmd), 0);
        nr = recv(sock, buf, sizeof(buf)-1, 0);
        if(nr > 0) { buf[nr]=0; printf("[Seedlink Bridge] Reply SELECT: %s\n", buf); }

        snprintf(cmd, sizeof(cmd), "DATA\r\n");
        printf("[Seedlink Bridge] Sending: %s", cmd);
        send(sock, cmd, strlen(cmd), 0);
        nr = recv(sock, buf, sizeof(buf)-1, 0);
        if (nr > 0) { buf[nr]=0; printf("[Seedlink Bridge] Reply DATA: %s\n", buf); }
        
        snprintf(cmd, sizeof(cmd), "END\r\n");
        printf("[Seedlink Bridge] Sending: %s", cmd);
        send(sock, cmd, strlen(cmd), 0);
        /* END starts streaming, no text response - binary packets follow immediately */
        
        printf("[Seedlink Bridge] Entering MiniSEED block reading loop...\n");
        fflush(stdout);
        int blocks_total = 0;
        while (1) {
            char hdr[SL_HDRSIZE];
            int n = recv(sock, hdr, SL_HDRSIZE, MSG_WAITALL);
            if (n == 0) {
                printf("[Seedlink Bridge] Server closed connection (recv=0).\n");
                break;
            }
            if (n < 0) {
                printf("[Seedlink Bridge] Error recv header: %s\n", strerror(errno));
                break;
            }
            if (n < SL_HDRSIZE) {
                printf("[Seedlink Bridge] Incomplete header: %d bytes\n", n);
                break;
            }
            /* Print raw SeedLink header for diagnosis */
            printf("[Seedlink Bridge] Header received (%d bytes): [%c%c%c%c%c%c%c%c] hex: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                   n,
                   (hdr[0]>=32&&hdr[0]<127)?hdr[0]:'.', (hdr[1]>=32&&hdr[1]<127)?hdr[1]:'.',
                   (hdr[2]>=32&&hdr[2]<127)?hdr[2]:'.', (hdr[3]>=32&&hdr[3]<127)?hdr[3]:'.',
                   (hdr[4]>=32&&hdr[4]<127)?hdr[4]:'.', (hdr[5]>=32&&hdr[5]<127)?hdr[5]:'.',
                   (hdr[6]>=32&&hdr[6]<127)?hdr[6]:'.', (hdr[7]>=32&&hdr[7]<127)?hdr[7]:'.',
                   (uint8_t)hdr[0],(uint8_t)hdr[1],(uint8_t)hdr[2],(uint8_t)hdr[3],
                   (uint8_t)hdr[4],(uint8_t)hdr[5],(uint8_t)hdr[6],(uint8_t)hdr[7]);
            fflush(stdout);
            
            /* Check SeedLink signature: must start with "SL" */
            if (hdr[0] != 'S' || hdr[1] != 'L') {
                printf("[Seedlink Bridge] WARNING: header does not start with 'SL', unexpected protocol.\n");
            }
            
            char rec[SL_RECSIZE];
            n = recv(sock, rec, SL_RECSIZE, MSG_WAITALL);
            if (n == 0) {
                printf("[Seedlink Bridge] Server closed connection while reading MiniSEED record.\n");
                break;
            }
            if (n < 0) {
                printf("[Seedlink Bridge] Error recv record: %s\n", strerror(errno));
                break;
            }
            blocks_total++;
            printf("[Seedlink Bridge] MiniSEED block #%d received (%d bytes), processing...\n", blocks_total, n);
            fflush(stdout);
            process_mseed(rec);
        }
        printf("[Seedlink Bridge] Loop terminated. Total blocks received: %d. Reconnecting in 5s...\n", blocks_total);
        close(sock);
        sleep(5);
    }
    return NULL;
}

void *heartbeat_thread(void *arg) {
    while (1) {
        sleep(2);
        pthread_mutex_lock(&clients_mutex);
        for (int i=0; i<MAX_CLIENTS; i++) {
            if (zejf_clients[i] != -1) {
                if (send(zejf_clients[i], "heartbeat\n", 10, MSG_NOSIGNAL) <= 0) {
                    close(zejf_clients[i]);
                    zejf_clients[i] = -1;
                }
            }
        }
        pthread_mutex_unlock(&clients_mutex);
    }
    return NULL;
}

typedef struct {
    int sock;
    int id;
} client_t;

void *client_handler(void *arg) {
    client_t *cli = (client_t*)arg;
    int sock = cli->sock;
    int id = cli->id;
    free(cli);
    
    char req[1024];
    FILE *f = fdopen(dup(sock), "r");
    if (!f) goto end;
    
    while (fgets(req, sizeof(req), f) != NULL) {
        if (strcmp(req, "realtime\n") == 0) {
            // El servidor Zejf no responde "realtime\n" directamente aquí, sino que cada bloque
            // de muestras ya empieza con "realtime\n".
            printf("Client %d send realtime.\n", id);
        } else if (strcmp(req, "getdata\n") == 0) {
            if (fgets(req, sizeof(req), f) == NULL) break;
            if (fgets(req, sizeof(req), f) == NULL) break;
            char empty_logs[] = "logs\n-2147483648\n";
            send(sock, empty_logs, strlen(empty_logs), MSG_NOSIGNAL);
        } else if (strcmp(req, "datahour_check\n") == 0) {
            if (fgets(req, sizeof(req), f) == NULL) break;
            if (fgets(req, sizeof(req), f) == NULL) break;
        } else if (strcmp(req, "heartbeat\n") == 0) {
            // No action needed, heartbeat loop sends heartbeat proactively
        }
    }
    
    fclose(f);
end:
    printf("Client %d disconnected.\n", id);
    pthread_mutex_lock(&clients_mutex);
    if (zejf_clients[id] == sock) {
        zejf_clients[id] = -1;
    }
    pthread_mutex_unlock(&clients_mutex);
    close(sock);
    return NULL;
}

int main() {
    for (int i=0; i<MAX_CLIENTS; i++) zejf_clients[i] = -1;
    load_config("../data/ranges.json");
    
    current_log_id = load_last_log_id();

    pthread_t persist_thread;
    pthread_create(&persist_thread, NULL, log_id_persist_thread, NULL);

    pthread_t sl_thread;
    pthread_create(&sl_thread, NULL, seedlink_client_thread, NULL);
    
    pthread_t hb_thread;
    pthread_create(&hb_thread, NULL, heartbeat_thread, NULL);
    
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(cfg.zejf_port);
    bind(server_fd, (struct sockaddr *)&address, sizeof(address));
    listen(server_fd, 3);
    
    printf("Zejf Bridge listening on port %d...\n", cfg.zejf_port);
    
    while (1) {
        int client_sock = accept(server_fd, NULL, NULL);
        if (client_sock < 0) continue;
        
        char msg[256];
        printf("New ZejfSeis client connected. Sending handshake...\n");
        int len = snprintf(msg, sizeof(msg), "compatibility_version:4\nsample_rate:%d\nerr_value:-2147483648\nlast_log_id:%llu\n", sample_rate, (unsigned long long)current_log_id);
        send(client_sock, msg, len, MSG_NOSIGNAL);
        
        int assigned_id = -1;
        pthread_mutex_lock(&clients_mutex);
        for (int i=0; i<MAX_CLIENTS; i++) {
            if (zejf_clients[i] == -1) {
                zejf_clients[i] = client_sock;
                assigned_id = i;
                break;
            }
        }
        pthread_mutex_unlock(&clients_mutex);
        
        if (assigned_id != -1) {
            printf("New client accepted, id %d\n", assigned_id);
            client_t *cli = malloc(sizeof(client_t));
            cli->sock = client_sock;
            cli->id = assigned_id;
            pthread_t cli_thread;
            pthread_create(&cli_thread, NULL, client_handler, cli);
            pthread_detach(cli_thread);
        } else {
            close(client_sock);
            printf("Client rejected, too many connections.\n");
        }
    }
    return 0;
}
