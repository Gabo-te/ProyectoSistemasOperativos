/*
 * ServidorTienda.c
 * Compilar: gcc -std=gnu11 -Wall -Wextra -pedantic ServidorTienda.c -o ServidorTienda -lpthread
 *
 * Correcciones:
 * - Uso de strdup (no g_strdup) para evitar dependencia a GLib.
 * - Trim seguro en el mismo buffer.
 * - Eliminación de separadores de miles (comas) en precio.
 * - GET_BRANDS devuelve marcas únicas y sin '|' final.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <ctype.h>

#define PORT 8080
#define BUFFER_SIZE 8192
#define MAX_PRODUCTOS 100
#define MAX_CARRITO  128

typedef struct {
    char* marca;
    char* modelo;
    char* specs;
    double precio;
    char* imagen;
} Producto;

static Producto inventario[MAX_PRODUCTOS];
static int inventario_size = 0;

/* Trim in-place: elimina espacios iniciales y finales y deja el resultado al inicio del buffer */
static char *trim_inplace(char *s) {
    if (!s) return s;
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    char *end = start + strlen(start);
    if (end != start) {
        end--; 
        while (end >= start && isspace((unsigned char)*end)) { *end = '\0'; end--; }
    }
    if (start != s) memmove(s, start, strlen(start) + 1);
    return s;
}

/* Remove thousands separators (commas) in-place */
static void remove_thousands_commas(char *s) {
    if (!s) return;
    char *r = s;
    for (char *p = s; *p; ++p) {
        if (*p == ',') continue;
        *r++ = *p;
    }
    *r = '\0';
}

static void cargar_inventario(const char* filename) {
    FILE* f = fopen(filename, "r");
    if (!f) {
        perror("inventario");
        exit(EXIT_FAILURE);
    }
    char line[4096];
    inventario_size = 0;
    while (fgets(line, sizeof(line), f) && inventario_size < MAX_PRODUCTOS) {
        /* remove trailing newline/carriage */
        line[strcspn(line, "\r\n")] = 0;

        /* tokenize by ';' */
        char *marca = strtok(line, ";");
        char *modelo = strtok(NULL, ";");
        char *specs = strtok(NULL, ";");
        char *precio = strtok(NULL, ";");
        char *imagen = strtok(NULL, ";");

        if (!marca || !modelo || !specs || !precio || !imagen) {
            /* malformed or incomplete line: skip */
            continue;
        }

        /* strdup tokens to own them */
        char *marca_trim = strdup(marca);
        char *modelo_trim = strdup(modelo);
        char *specs_trim = strdup(specs);
        char *precio_trim = strdup(precio);
        char *imagen_trim = strdup(imagen);

        if (!marca_trim || !modelo_trim || !specs_trim || !precio_trim || !imagen_trim) {
            /* allocation failure: free what we have and break */
            free(marca_trim); free(modelo_trim); free(specs_trim); free(precio_trim); free(imagen_trim);
            break;
        }

        /* trim each field in-place (and normalize to start of buffer) */
        trim_inplace(marca_trim);
        trim_inplace(modelo_trim);
        trim_inplace(specs_trim);
        trim_inplace(precio_trim);
        trim_inplace(imagen_trim);

        /* remove comma thousand separators in price, e.g. "12,999.00" -> "12999.00" */
        remove_thousands_commas(precio_trim);

        double price_val = atof(precio_trim);

        /* store pointers (owned) */
        inventario[inventario_size].marca  = marca_trim;
        inventario[inventario_size].modelo = modelo_trim;
        inventario[inventario_size].specs  = specs_trim;
        inventario[inventario_size].precio = price_val;
        inventario[inventario_size].imagen = imagen_trim;

        inventario_size++;
    }
    fclose(f);
    printf("[SERVIDOR] Inventario cargado: %d productos\n", inventario_size);
}

/* find by modelo exact match */
static Producto* find_model(const char* modelo) {
    for (int i = 0; i < inventario_size; ++i)
        if (strcmp(inventario[i].modelo, modelo) == 0)
            return &inventario[i];
    return NULL;
}

/* check brand exists in list */
static int brand_already(char** list, int count, const char* brand) {
    for (int i = 0; i < count; ++i)
        if (strcmp(list[i], brand) == 0) return 1;
    return 0;
}

static void* handle_client(void* arg) {
    int sock = *(int*)arg;
    free(arg);

    Producto* carrito[MAX_CARRITO];
    int carrito_size = 0;

    char buffer[BUFFER_SIZE];
    int n;
    printf("[SERVIDOR] Cliente conectado FD=%d\n", sock);

    while ((n = recv(sock, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[n] = '\0';
        buffer[strcspn(buffer, "\r\n")] = 0;
        if (!*buffer) continue;

        char response[BUFFER_SIZE];
        response[0] = '\0';

        if (strcmp(buffer, "GET_BRANDS") == 0) {
            /* build unique brands list */
            char* brands_seen[MAX_PRODUCTOS];
            int seen = 0;
            for (int i = 0; i < inventario_size; ++i) {
                const char *b = inventario[i].marca;
                if (!brand_already(brands_seen, seen, b)) {
                    brands_seen[seen++] = (char*)b;
                }
            }
            /* join with '|' without trailing '|' */
            for (int i = 0; i < seen; ++i) {
                strncat(response, brands_seen[i], sizeof(response)-1 - strlen(response));
                if (i < seen - 1) strncat(response, "|", sizeof(response)-1 - strlen(response));
            }
            strncat(response, "\n", sizeof(response)-1 - strlen(response));
        }
        else if (strncmp(buffer, "GET_MODELS:", 11) == 0) {
            const char* brand = buffer + 11;
            for (int i = 0; i < inventario_size; ++i) {
                if (strcmp(inventario[i].marca, brand) == 0) {
                    char linebuf[1024];
                    snprintf(linebuf, sizeof(linebuf), "%s|%s|%.2f|%s\n",
                             inventario[i].modelo,
                             inventario[i].specs,
                             inventario[i].precio,
                             inventario[i].imagen);
                    if (strlen(response) + strlen(linebuf) < sizeof(response)-1)
                        strcat(response, linebuf);
                }
            }
            if (!*response) strcpy(response, "\n");
        }
        else if (strncmp(buffer, "ADD_TO_CART:", 12) == 0) {
            const char* modelo = buffer + 12;
            if (carrito_size >= MAX_CARRITO) {
                strcpy(response, "ERROR: Carrito lleno\n");
            } else {
                Producto* p = find_model(modelo);
                if (p) {
                    carrito[carrito_size++] = p;
                    strcpy(response, "OK\n");
                } else {
                    strcpy(response, "ERROR: Modelo no encontrado\n");
                }
            }
        }
        else if (strcmp(buffer, "GET_CART_ITEMS") == 0) {
            if (carrito_size == 0) {
                strcpy(response, "EMPTY\n");
            } else {
                for (int i = 0; i < carrito_size; ++i) {
                    char linebuf[1024];
                    snprintf(linebuf, sizeof(linebuf), "%s|%s|%s|%.2f|%s\n",
                             carrito[i]->modelo,
                             carrito[i]->marca,
                             carrito[i]->specs,
                             carrito[i]->precio,
                             carrito[i]->imagen);
                    if (strlen(response) + strlen(linebuf) < sizeof(response)-1)
                        strcat(response, linebuf);
                }
            }
        }
        else if (strncmp(buffer, "CHECKOUT:", 9) == 0) {
            const char* metodo = buffer + 9;
            double total = 0.0;
            for (int i = 0; i < carrito_size; ++i) total += carrito[i]->precio;
            time_t now = time(NULL);
            struct tm tmv;
            localtime_r(&now, &tmv);
            char fecha[32];
            strftime(fecha, sizeof(fecha), "%Y-%m-%d %H:%M:%S", &tmv);
            snprintf(response, sizeof(response), "OK|%s|%.2f\n", fecha, total);
            for (int i = 0; i < carrito_size; ++i) {
                char linebuf[1024];
                snprintf(linebuf, sizeof(linebuf), "%s|%s|%s|%.2f|%s\n",
                         carrito[i]->modelo,
                         carrito[i]->marca,
                         carrito[i]->specs,
                         carrito[i]->precio,
                         carrito[i]->imagen);
                if (strlen(response) + strlen(linebuf) < sizeof(response)-1)
                    strcat(response, linebuf);
            }
            carrito_size = 0;
            (void)metodo;
        }
        else {
            strcpy(response, "COMANDO_NO_VALIDO\n");
        }

        send(sock, response, strlen(response), 0);
        memset(buffer, 0, sizeof(buffer));
    }

    close(sock);
    printf("[SERVIDOR] Cliente desconectado FD=%d\n", sock);
    return NULL;
}

int main() {
    cargar_inventario("InvetarioCelulares.csv");

    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(server_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    if (listen(server_socket, 8) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    printf("[SERVIDOR] Escuchando en %d\n", PORT);

    while (1) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int client_fd = accept(server_socket, (struct sockaddr*)&caddr, &clen);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }
        int* fdptr = malloc(sizeof(int));
        *fdptr = client_fd;
        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, fdptr) == 0) {
            pthread_detach(tid);
        } else {
            perror("pthread_create");
            close(client_fd);
            free(fdptr);
        }
    }

    close(server_socket);
    for (int i = 0; i < inventario_size; ++i) {
        free(inventario[i].marca);
        free(inventario[i].modelo);
        free(inventario[i].specs);
        free(inventario[i].imagen);
    }
    return 0;
}