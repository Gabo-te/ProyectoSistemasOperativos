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
#include <stdbool.h>

#define PORT 5000
#define BUFFER_SIZE 8192
#define MAX_PRODUCTOS 100
#define MAX_CARRITO  128
#define MAX_USUARIOS 128
#define INVENTARIO_FILE "InvetarioCelulares.csv"
#define USUARIOS_FILE   "Usuarios.csv"

typedef struct {
    char* marca;
    char* modelo;
    char* specs;
    double precio;
    char* imagen;
    bool activo;
} Producto;

static Producto inventario[MAX_PRODUCTOS];
static int inventario_size = 0;

typedef struct {
    char *username;
    char *password;
    char *role;
    bool is_admin;
} Usuario;

static Usuario usuarios[MAX_USUARIOS];
static int usuarios_size = 0;

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
        inventario[inventario_size].activo = true;

        inventario_size++;
    }
    fclose(f);
    printf("[SERVIDOR] Inventario cargado: %d productos\n", inventario_size);
}

static void persist_inventory(void) {
    FILE *f = fopen(INVENTARIO_FILE, "w");
    if (!f) {
        perror("inventario_save");
        return;
    }
    for (int i = 0; i < inventario_size; ++i) {
        if (!inventario[i].activo) continue;
        fprintf(f, "%s;%s;%s;%.2f;%s\n",
                inventario[i].marca,
                inventario[i].modelo,
                inventario[i].specs,
                inventario[i].precio,
                inventario[i].imagen);
    }
    fclose(f);
}

static void cargar_usuarios(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "[SERVIDOR] Advertencia: no se pudo abrir %s; sin usuarios precargados.\n", filename);
        usuarios_size = 0;
        return;
    }

    char line[1024];
    usuarios_size = 0;
    while (fgets(line, sizeof(line), f) && usuarios_size < MAX_USUARIOS) {
        line[strcspn(line, "\r\n")] = 0;
        char *user = strtok(line, ";");
        char *pass = strtok(NULL, ";");
        char *role = strtok(NULL, ";");
        if (!user || !pass) continue;

        char *user_dup = strdup(user);
        char *pass_dup = strdup(pass);
        char *role_dup = NULL;
        if (role) {
            role_dup = strdup(role);
        } else {
            const char *fallback = (strcmp(user, "admin") == 0) ? "admin" : "cliente";
            role_dup = strdup(fallback);
        }
        if (!user_dup || !pass_dup || !role_dup) {
            free(user_dup);
            free(pass_dup);
            free(role_dup);
            continue;
        }

        trim_inplace(user_dup);
        trim_inplace(pass_dup);
        trim_inplace(role_dup);

        if (*role_dup == '\0' && strcmp(user_dup, "admin") == 0) {
            free(role_dup);
            role_dup = strdup("admin");
        } else if (*role_dup == '\0') {
            free(role_dup);
            role_dup = strdup("cliente");
        }

        usuarios[usuarios_size].username = user_dup;
        usuarios[usuarios_size].password = pass_dup;
        usuarios[usuarios_size].role = role_dup;
        usuarios[usuarios_size].is_admin = (strcmp(role_dup, "admin") == 0);
        usuarios_size++;
    }
    fclose(f);
    printf("[SERVIDOR] Usuarios cargados: %d\n", usuarios_size);
}

static Usuario *find_usuario(const char *username) {
    for (int i = 0; i < usuarios_size; ++i) {
        if (strcmp(usuarios[i].username, username) == 0) return &usuarios[i];
    }
    return NULL;
}

static bool append_user_record(const char *username, const char *password, const char *role) {
    FILE *f = fopen(USUARIOS_FILE, "a");
    if (!f) {
        perror("usuarios_append");
        return false;
    }
    int written = fprintf(f, "%s;%s;%s\n", username, password, role);
    fclose(f);
    return written > 0;
}

static bool add_user(const char *username, const char *password, const char *role, bool persist) {
    if (find_usuario(username)) return false;
    if (usuarios_size >= MAX_USUARIOS) return false;

    char *user_dup = strdup(username);
    char *pass_dup = strdup(password);
    char *role_dup = strdup(role);
    if (!user_dup || !pass_dup || !role_dup) {
        free(user_dup);
        free(pass_dup);
        free(role_dup);
        return false;
    }

    if (persist && !append_user_record(username, password, role)) {
        free(user_dup);
        free(pass_dup);
        free(role_dup);
        return false;
    }

    usuarios[usuarios_size].username = user_dup;
    usuarios[usuarios_size].password = pass_dup;
    usuarios[usuarios_size].role = role_dup;
    usuarios[usuarios_size].is_admin = (strcmp(role_dup, "admin") == 0);
    usuarios_size++;
    return true;
}

static void persist_users(void) {
    FILE *f = fopen(USUARIOS_FILE, "w");
    if (!f) {
        perror("usuarios_save");
        return;
    }
    for (int i = 0; i < usuarios_size; ++i) {
        fprintf(f, "%s;%s;%s\n",
                usuarios[i].username,
                usuarios[i].password,
                usuarios[i].role ? usuarios[i].role : "cliente");
    }
    fclose(f);
}

static void ensure_default_admin(void) {
    Usuario *admin = find_usuario("admin");
    if (admin) {
        if (!admin->is_admin) {
            char *new_role = strdup("admin");
            if (!new_role) return;
            free(admin->role);
            admin->role = new_role;
            admin->is_admin = true;
            persist_users();
            printf("[SERVIDOR] Cuenta admin actualizada con privilegios.\n");
        }
        return;
    }
    if (!add_user("admin", "admin123", "admin", true)) {
        fprintf(stderr, "[SERVIDOR] No se pudo crear cuenta admin por defecto.\n");
    } else {
        printf("[SERVIDOR] Cuenta admin por defecto generada.\n");
    }
}

/* find by modelo exact match */
static Producto* find_model(const char* modelo) {
    for (int i = 0; i < inventario_size; ++i)
        if (inventario[i].activo && strcmp(inventario[i].modelo, modelo) == 0)
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
    char current_user[128] = "";
    char current_role[16] = "cliente";
    bool logged_in = false;

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
                if (!inventario[i].activo) continue;
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
                if (!inventario[i].activo) continue;
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
            int write_idx = 0;
            for (int i = 0; i < carrito_size; ++i) {
                if (carrito[i] && carrito[i]->activo) {
                    carrito[write_idx++] = carrito[i];
                }
            }
            carrito_size = write_idx;

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
            if (!logged_in) {
                strcpy(response, "ERROR:LOGIN_REQUIRED\n");
                goto send_reply;
            }
            const char* metodo = buffer + 9;
            int write_idx = 0;
            for (int i = 0; i < carrito_size; ++i) {
                if (carrito[i] && carrito[i]->activo) {
                    carrito[write_idx++] = carrito[i];
                }
            }
            carrito_size = write_idx;
            if (carrito_size == 0) {
                strcpy(response, "ERROR:CART_EMPTY\n");
                goto send_reply;
            }
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
        else if (strncmp(buffer, "LOGIN:", 6) == 0) {
            const char *payload = buffer + 6;
            char copy[512];
            strncpy(copy, payload, sizeof(copy) - 1);
            copy[sizeof(copy) - 1] = '\0';
            char *sep = strchr(copy, '|');
            if (!sep) {
                strcpy(response, "ERROR\n");
            } else {
                *sep = '\0';
                const char *user = copy;
                const char *pass = sep + 1;
                Usuario *u = find_usuario(user);
                if (u && strcmp(u->password, pass) == 0) {
                    snprintf(response, sizeof(response), "OK|%s\n", u->role);
                    logged_in = true;
                    strncpy(current_user, u->username, sizeof(current_user) - 1);
                    current_user[sizeof(current_user) - 1] = '\0';
                    strncpy(current_role, u->role, sizeof(current_role) - 1);
                    current_role[sizeof(current_role) - 1] = '\0';
                } else {
                    strcpy(response, "ERROR\n");
                }
            }
        }
        else if (strncmp(buffer, "REGISTER:", 9) == 0) {
            const char *payload = buffer + 9;
            char copy[512];
            strncpy(copy, payload, sizeof(copy) - 1);
            copy[sizeof(copy) - 1] = '\0';
            char *sep = strchr(copy, '|');
            if (!sep) {
                strcpy(response, "ERROR|Formato invalido\n");
            } else {
                *sep = '\0';
                const char *user = copy;
                const char *pass = sep + 1;
                if (find_usuario(user)) {
                    strcpy(response, "ERROR|Usuario existente\n");
                } else if (strlen(user) < 3 || strlen(pass) < 4) {
                    strcpy(response, "ERROR|Datos demasiado cortos\n");
                } else if (strpbrk(user, "|\r\n") || strpbrk(pass, "|\r\n")) {
                    strcpy(response, "ERROR|Caracteres invalidos\n");
                } else if (!add_user(user, pass, "cliente", true)) {
                    strcpy(response, "ERROR|No se pudo registrar\n");
                } else {
                    strcpy(response, "OK\n");
                }
            }
        }
        else if (strncmp(buffer, "REMOVE_PRODUCT:", 15) == 0) {
            if (!logged_in || strcmp(current_role, "admin") != 0) {
                strcpy(response, "ERROR|SIN_PERMISOS\n");
            } else {
                const char *modelo = buffer + 15;
                Producto *p = find_model(modelo);
                if (!p) {
                    strcpy(response, "ERROR|NO_ENCONTRADO\n");
                } else {
                    p->activo = false;
                    persist_inventory();
                    strcpy(response, "OK\n");
                }
            }
        }
        else if (strcmp(buffer, "GET_ALL_PRODUCTS") == 0) {
            if (!logged_in || strcmp(current_role, "admin") != 0) {
                strcpy(response, "ERROR|SIN_PERMISOS\n");
            } else {
                bool any = false;
                for (int i = 0; i < inventario_size; ++i) {
                    if (!inventario[i].activo) continue;
                    any = true;
                    char linebuf[1024];
                    snprintf(linebuf, sizeof(linebuf), "%s|%s|%s|%.2f\n",
                             inventario[i].marca,
                             inventario[i].modelo,
                             inventario[i].specs,
                             inventario[i].precio);
                    if (strlen(response) + strlen(linebuf) < sizeof(response)-1)
                        strcat(response, linebuf);
                }
                if (!any) strcpy(response, "EMPTY\n");
            }
        }
        else {
            strcpy(response, "COMANDO_NO_VALIDO\n");
        }

send_reply:
        send(sock, response, strlen(response), 0);
        memset(buffer, 0, sizeof(buffer));
    }

    close(sock);
    printf("[SERVIDOR] Cliente desconectado FD=%d\n", sock);
    return NULL;
}

int main() {
    cargar_inventario(INVENTARIO_FILE);
    cargar_usuarios(USUARIOS_FILE);
    ensure_default_admin();

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
    for (int i = 0; i < usuarios_size; ++i) {
        free(usuarios[i].username);
        free(usuarios[i].password);
        free(usuarios[i].role);
    }
    return 0;
}
