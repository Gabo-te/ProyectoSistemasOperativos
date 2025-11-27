/*
 * ClienteTienda.c
 * Compilar:
 * gcc -std=gnu11 -Wall -Wextra -pedantic ClienteTienda.c -o ClienteTienda $(pkg-config --cflags --libs gtk+-3.0 gdk-pixbuf-2.0)
 *
 * Mejoras implementadas:
 * - Protocolo optimizado (GET_MODELS ya incluye specs).
 * - Eliminado botón "Ver Detalles".
 * - Total parcial del carrito.
 * - Diálogo de pago con formateo de tarjeta, CVV y fecha.
 * - Folio OXXO + botón "Copiar".
 * - Guardar ticket a CSV/TXT.
 * - CSS tipo tienda en línea (style.css).
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <ctype.h>
#include <time.h>

#define SERVER_PORT 5000
#define BUFFER_SIZE 8192

/* Globals */
static int server_socket = -1;
static GtkWidget *main_stack;
static GtkWidget *g_cart_list_box;
static GtkWidget *g_cart_total_label;
static GtkWidget *g_ticket_list_box;
static GtkWidget *g_ticket_date_label;
static GtkWidget *g_ticket_total_label;
static GtkWidget *g_ticket_save_button;
static GtkWidget *g_login_username_entry;
static GtkWidget *g_login_password_entry;
static GtkWidget *g_login_error_label;
static GtkWidget *g_welcome_label;
static GtkWidget *g_cart_button;
static GtkWidget *g_admin_button;
static GtkWidget *g_admin_list_box;
static GtkWidget *g_admin_status_label;

/* Para folio OXXO */
static GtkWidget *g_folio_row = NULL;

/* Último ticket bruto para exportar */
static char *g_last_ticket_raw = NULL;
static char *g_current_user = NULL;
static char *g_user_role = NULL;

typedef struct {
    GtkWidget *filter_modelo;
    GtkWidget *filter_marca;
    GtkWidget *filter_procesador;
    GtkWidget *cart_list_box;
} CartFilterData;

static CartFilterData g_cart_filter_data;

/* Prototipos */
static void on_start_shopping_clicked(GtkButton *button, gpointer user_data);
static void on_brand_clicked(GtkButton *button, GtkWidget *model_container);
static void on_add_to_cart_clicked(GtkButton *button, const char *model_name);
static void on_back_to_brands_clicked(GtkButton *button, gpointer user_data);
static void on_view_cart_clicked(GtkButton *button, gpointer user_data);
static void on_back_to_shop_clicked(GtkButton *button, gpointer user_data);
static void on_cart_filter_changed(GtkEditable *editable, gpointer user_data);
static void on_checkout_clicked(GtkButton *button, gpointer user_data);
static void payment_radio_toggled(GtkToggleButton *toggle, gpointer user_data);
static void on_checkout_submit(GtkDialog *dialog, gint response_id, gpointer user_data);
static void cleanup_and_exit(GtkWidget *widget, gpointer user_data);
static void on_save_ticket_clicked(GtkButton *button, gpointer user_data);
static void on_copy_folio_clicked(GtkButton *button, gpointer user_data);
static void on_login_clicked(GtkButton *button, gpointer user_data);
static gboolean attempt_login(const char *username, const char *password, char **role_out, char **error_out);
static void on_register_clicked(GtkButton *button, gpointer user_data);
static gboolean attempt_register(const char *username, const char *password, char **error_out);
static void on_admin_manage_clicked(GtkButton *button, gpointer user_data);
static void refresh_admin_products(void);
static void on_admin_remove_clicked(GtkButton *button, gpointer user_data);

/* Formateo entradas tarjeta */
static void on_card_number_changed(GtkEditable *editable, gpointer user_data);
static void on_card_exp_changed(GtkEditable *editable, gpointer user_data);
static void on_card_cvv_changed(GtkEditable *editable, gpointer user_data);

/* Helpers */
static void show_net_error_and_keep_ui(const char* msg) {
    GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
        GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", msg);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static char* send_command(const char* command) {
    static char response_buffer[BUFFER_SIZE];
    memset(response_buffer, 0, sizeof(response_buffer));
    if (server_socket < 0) return NULL;
    if (send(server_socket, command, strlen(command), 0) < 0) {
        perror("send");
        show_net_error_and_keep_ui("Error al enviar comando al servidor.");
        return NULL;
    }
    int bytes_received = recv(server_socket, response_buffer, sizeof(response_buffer) - 1, 0);
    if (bytes_received <= 0) {
        if (bytes_received == 0)
            show_net_error_and_keep_ui("El servidor cerró la conexión.");
        else {
            perror("recv");
            show_net_error_and_keep_ui("Error al recibir respuesta del servidor.");
        }
        return NULL;
    }
    response_buffer[bytes_received] = '\0';
    return response_buffer;
}

static GdkPixbuf *scale_pixbuf(const char *filename, int w, int h) {
    GdkPixbuf *pixbuf = NULL;
    GError *error = NULL;
    pixbuf = gdk_pixbuf_new_from_file(filename, &error);
    if (error != NULL) {
        g_clear_error(&error);
        pixbuf = gdk_pixbuf_new_from_file("images/placeholder.png", &error);
        if (error != NULL) {
            g_clear_error(&error);
            return NULL;
        }
    }
    int src_w = gdk_pixbuf_get_width(pixbuf);
    int src_h = gdk_pixbuf_get_height(pixbuf);
    if (src_w == 0 || src_h == 0) {
        g_object_unref(pixbuf);
        return NULL;
    }
    double scale_w = (double)w / src_w;
    double scale_h = (double)h / src_h;
    double scale = scale_w < scale_h ? scale_w : scale_h;
    int new_w = (int)(src_w * scale);
    int new_h = (int)(src_h * scale);
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pixbuf, new_w, new_h, GDK_INTERP_BILINEAR);
    g_object_unref(pixbuf);
    return scaled;
}

static void clear_container(GtkWidget *container) {
    if (!GTK_IS_CONTAINER(container)) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(container));
    for (GList *iter = children; iter != NULL; iter = g_list_next(iter))
        gtk_widget_destroy(GTK_WIDGET(iter->data));
    g_list_free(children);
}

static char *str_to_lower(const char *str) {
    if (!str) return NULL;
    char *lower = g_strdup(str);
    for (int i = 0; lower[i]; i++)
        lower[i] = (char)tolower((unsigned char)lower[i]);
    return lower;
}

/* CSS */
static void load_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    GError *error = NULL;
    if (!gtk_css_provider_load_from_path(provider, "style.css", &error)) {
        if (error) {
            g_warning("CSS error: %s", error->message);
            g_clear_error(&error);
        }
    }
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_USER);
    g_object_unref(provider);
}

/* MAIN */
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <IP_del_servidor>\n", argv[0]);
        return 1;
    }
    const char *server_ip = argv[1];

    struct sockaddr_in server_addr;
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);
    if (connect(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        return 1;
    }
    printf("Conectado al servidor %s\n", server_ip);

    gtk_init(&argc, &argv);
    load_css();

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Celu-Mercado");
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 700);
    g_signal_connect(window, "destroy", G_CALLBACK(cleanup_and_exit), NULL);

    GtkWidget *header_bar = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header_bar), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(header_bar), "Celu-Mercado");

    /* Botón Administrar */
    g_admin_button = gtk_button_new_with_label("Administrar");
    gtk_style_context_add_class(gtk_widget_get_style_context(g_admin_button), "header-btn");
    gtk_widget_set_visible(g_admin_button, FALSE);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header_bar), g_admin_button);
    g_signal_connect(g_admin_button, "clicked", G_CALLBACK(on_admin_manage_clicked), NULL);

    /* Botón Carrito: icono + texto */
    GtkWidget *cart_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *cart_icon = gtk_image_new_from_icon_name("view-list-symbolic", GTK_ICON_SIZE_MENU);
    GtkWidget *cart_label = gtk_label_new("Carrito");
    gtk_box_pack_start(GTK_BOX(cart_box), cart_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cart_box), cart_label, FALSE, FALSE, 0);

    g_cart_button = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(g_cart_button), cart_box);
    gtk_widget_set_sensitive(g_cart_button, FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(g_cart_button), "header-cart-btn");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header_bar), g_cart_button);
    g_signal_connect(g_cart_button, "clicked", G_CALLBACK(on_view_cart_clicked), NULL);

    gtk_window_set_titlebar(GTK_WINDOW(window), header_bar);

    main_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(main_stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_container_add(GTK_CONTAINER(window), main_stack);

    /* Login */
    GtkWidget *login_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_halign(login_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(login_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(login_box, 40);
    gtk_widget_set_margin_bottom(login_box, 40);
    GtkWidget *login_title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(login_title), "<span size='xx-large'>Inicia sesión</span>");
    gtk_style_context_add_class(gtk_widget_get_style_context(login_title), "title-label");
    g_login_username_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_login_username_entry), "Usuario");
    gtk_entry_set_activates_default(GTK_ENTRY(g_login_username_entry), TRUE);
    g_login_password_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_login_password_entry), "Contraseña");
    gtk_entry_set_visibility(GTK_ENTRY(g_login_password_entry), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(g_login_password_entry), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_activates_default(GTK_ENTRY(g_login_password_entry), TRUE);
    GtkWidget *login_button = gtk_button_new_with_label("Ingresar");
    gtk_widget_set_can_default(login_button, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(login_button), "suggested-action");
    g_signal_connect(login_button, "clicked", G_CALLBACK(on_login_clicked), NULL);
    GtkWidget *register_button = gtk_button_new_with_label("Crear cuenta");
    gtk_widget_set_halign(register_button, GTK_ALIGN_CENTER);
    gtk_button_set_relief(GTK_BUTTON(register_button), GTK_RELIEF_NONE);
    g_signal_connect(register_button, "clicked", G_CALLBACK(on_register_clicked), NULL);
    g_login_error_label = gtk_label_new("Credenciales incorrectas");
    gtk_widget_set_halign(g_login_error_label, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(g_login_error_label), "error-text");
    gtk_widget_hide(g_login_error_label);
    gtk_box_pack_start(GTK_BOX(login_box), login_title, FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(login_box), g_login_username_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(login_box), g_login_password_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(login_box), login_button, FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(login_box), register_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(login_box), g_login_error_label, FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(main_stack), login_box, "login_view");

    /* Welcome */
    GtkWidget *welcome_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_set_halign(welcome_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(welcome_box, GTK_ALIGN_CENTER);
    g_welcome_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(g_welcome_label), "<span size='xx-large'>Bienvenido a Celu-Mercado</span>");
    gtk_style_context_add_class(gtk_widget_get_style_context(g_welcome_label), "title-label");
    GtkWidget *start_button = gtk_button_new_with_label("Empezar a Comprar");
    gtk_style_context_add_class(gtk_widget_get_style_context(start_button), "suggested-action");
    g_signal_connect(start_button, "clicked", G_CALLBACK(on_start_shopping_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(welcome_box), g_welcome_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(welcome_box), start_button, FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(main_stack), welcome_box, "welcome_view");

    /* Brands */
    GtkWidget *brands_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(brands_page, 20);
    gtk_widget_set_margin_start(brands_page, 20);
    gtk_widget_set_margin_end(brands_page, 20);

    GtkWidget *brands_title = gtk_label_new("Explora por marca");
    gtk_widget_set_halign(brands_title, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(brands_title), "subtitle-label");
    gtk_box_pack_start(GTK_BOX(brands_page), brands_title, FALSE, FALSE, 0);

    GtkWidget *brands_flow = gtk_flow_box_new();
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(brands_flow), 3);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(brands_flow), GTK_SELECTION_NONE);
    gtk_widget_set_margin_top(brands_flow, 20);
    gtk_widget_set_margin_start(brands_flow, 20);
    gtk_widget_set_margin_end(brands_flow, 20);
    gtk_box_pack_start(GTK_BOX(brands_page), brands_flow, TRUE, TRUE, 0);
    gtk_stack_add_named(GTK_STACK(main_stack), brands_page, "brands_view");

    /* Models */
    GtkWidget *models_page_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *models_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(models_scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(models_scrolled, TRUE);
    GtkWidget *model_flow = gtk_flow_box_new();
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(model_flow), 4);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(model_flow), GTK_SELECTION_NONE);
    gtk_widget_set_margin_top(model_flow, 20);
    gtk_widget_set_margin_start(model_flow, 20);
    gtk_widget_set_margin_end(model_flow, 20);
    gtk_container_add(GTK_CONTAINER(models_scrolled), model_flow);

    GtkWidget *back_to_brands_button = gtk_button_new_with_label("← Volver a Marcas");
    gtk_widget_set_halign(back_to_brands_button, GTK_ALIGN_START);
    gtk_widget_set_margin_start(back_to_brands_button, 10);
    gtk_widget_set_margin_top(back_to_brands_button, 10);
    g_signal_connect(back_to_brands_button, "clicked", G_CALLBACK(on_back_to_brands_clicked), NULL);

    gtk_box_pack_start(GTK_BOX(models_page_box), back_to_brands_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(models_page_box), models_scrolled, TRUE, TRUE, 0);
    gtk_stack_add_named(GTK_STACK(main_stack), models_page_box, "models_view");

    /* Obtener marcas */
    char *brands_response = send_command("GET_BRANDS");
    if (brands_response) {
        char *copy = g_strdup(brands_response);
        char *saveptr;
        char *brand = strtok_r(copy, "|", &saveptr);
        while (brand) {
            if (*brand != '\0') {
                char slug[256]; int si = 0;
                for (const char *p = brand; *p && si < (int)sizeof(slug)-1; ++p) {
                    unsigned char c = (unsigned char)*p;
                    if (isspace(c)) { if (si && slug[si-1] != '_') slug[si++] = '_'; }
                    else if (isalnum(c) || c == '_' ) slug[si++] = (char)tolower(c);
                }
                if (si == 0) {
                    strncpy(slug, brand, sizeof(slug)-1);
                    slug[sizeof(slug)-1] = '\0';
                } else slug[si] = '\0';

                char logo_path[512];
                snprintf(logo_path, sizeof(logo_path), "images/logos/%s.png", slug);

                GdkPixbuf *logo_px = NULL;
                GError *err = NULL;
                logo_px = gdk_pixbuf_new_from_file(logo_path, &err);
                if (err) { g_clear_error(&err); logo_px = NULL; }

                GtkWidget *btn = NULL;
                if (logo_px) {
                    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(logo_px, 140, 70, GDK_INTERP_BILINEAR);
                    g_object_unref(logo_px);
                    GtkWidget *img = gtk_image_new_from_pixbuf(scaled);
                    if (scaled) g_object_unref(scaled);
                    btn = gtk_button_new();
                    gtk_button_set_image(GTK_BUTTON(btn), img);
                    gtk_widget_set_tooltip_text(btn, brand);
                } else {
                    btn = gtk_button_new_with_label(brand);
                    gtk_widget_set_size_request(btn, 150, 70);
                }

                g_object_set_data_full(G_OBJECT(btn), "brand-name", g_strdup(brand), g_free);
                g_signal_connect(btn, "clicked", G_CALLBACK(on_brand_clicked), model_flow);

                gtk_flow_box_insert(GTK_FLOW_BOX(brands_flow), btn, -1);
            }
            brand = strtok_r(NULL, "|", &saveptr);
        }
        g_free(copy);
    } else {
        GtkWidget *err = gtk_label_new("Error: no se pudo cargar marcas.");
        gtk_flow_box_insert(GTK_FLOW_BOX(brands_flow), err, -1);
    }

    /* Cart view */
    GtkWidget *cart_page_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(cart_page_box, 10);
    gtk_widget_set_margin_end(cart_page_box, 10);
    gtk_widget_set_margin_top(cart_page_box, 10);
    gtk_widget_set_margin_bottom(cart_page_box, 10);

    GtkWidget *back_button = gtk_button_new_with_label("← Volver a la Tienda");
    gtk_widget_set_halign(back_button, GTK_ALIGN_START);
    g_signal_connect(back_button, "clicked", G_CALLBACK(on_back_to_shop_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(cart_page_box), back_button, FALSE, FALSE, 0);

    GtkWidget *cart_title = gtk_label_new("Tu carrito de compras");
    gtk_widget_set_halign(cart_title, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(cart_title), "subtitle-label");
    gtk_box_pack_start(GTK_BOX(cart_page_box), cart_title, FALSE, FALSE, 2);

    GtkWidget *filter_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(cart_page_box), filter_box, FALSE, FALSE, 5);
    GtkWidget *filter_modelo_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(filter_modelo_entry), "Filtrar por modelo...");
    GtkWidget *filter_marca_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(filter_marca_entry), "Filtrar por marca...");
    GtkWidget *filter_procesador_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(filter_procesador_entry), "Filtrar por procesador...");
    gtk_style_context_add_class(gtk_widget_get_style_context(filter_modelo_entry), "filter-entry");
    gtk_style_context_add_class(gtk_widget_get_style_context(filter_marca_entry), "filter-entry");
    gtk_style_context_add_class(gtk_widget_get_style_context(filter_procesador_entry), "filter-entry");
    gtk_box_pack_start(GTK_BOX(filter_box), filter_modelo_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(filter_box), filter_marca_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(filter_box), filter_procesador_entry, TRUE, TRUE, 0);

    GtkWidget *cart_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(cart_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(cart_sw, TRUE);
    g_cart_list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_cart_list_box), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(cart_sw), g_cart_list_box);
    gtk_box_pack_start(GTK_BOX(cart_page_box), cart_sw, TRUE, TRUE, 0);

    GtkWidget *cart_total_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    g_cart_total_label = gtk_label_new("Total carrito: $ 0.00");
    gtk_style_context_add_class(gtk_widget_get_style_context(g_cart_total_label), "total-label");
    gtk_box_pack_end(GTK_BOX(cart_total_box), g_cart_total_label, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(cart_page_box), cart_total_box, FALSE, FALSE, 5);

    GtkWidget *proceed_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *proceed_button = gtk_button_new_with_label("Proceder al pago");
    gtk_style_context_add_class(gtk_widget_get_style_context(proceed_button), "suggested-action");
    g_signal_connect(proceed_button, "clicked", G_CALLBACK(on_checkout_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(proceed_box), proceed_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cart_page_box), proceed_box, FALSE, FALSE, 5);

    g_cart_filter_data.filter_modelo = filter_modelo_entry;
    g_cart_filter_data.filter_marca = filter_marca_entry;
    g_cart_filter_data.filter_procesador = filter_procesador_entry;
    g_cart_filter_data.cart_list_box = g_cart_list_box;
    g_signal_connect(filter_modelo_entry, "changed", G_CALLBACK(on_cart_filter_changed), &g_cart_filter_data);
    g_signal_connect(filter_marca_entry, "changed", G_CALLBACK(on_cart_filter_changed), &g_cart_filter_data);
    g_signal_connect(filter_procesador_entry, "changed", G_CALLBACK(on_cart_filter_changed), &g_cart_filter_data);

    gtk_stack_add_named(GTK_STACK(main_stack), cart_page_box, "cart_view");

    /* Ticket view */
    GtkWidget *ticket_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(ticket_page, 10);
    gtk_widget_set_margin_end(ticket_page, 10);
    gtk_widget_set_margin_top(ticket_page, 10);
    gtk_widget_set_margin_bottom(ticket_page, 10);

    GtkWidget *ticket_back = gtk_button_new_with_label("← Volver a la Tienda");
    g_signal_connect(ticket_back, "clicked", G_CALLBACK(on_back_to_shop_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(ticket_page), ticket_back, FALSE, FALSE, 0);

    GtkWidget *ticket_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(ticket_container), "ticket-box");
    gtk_box_pack_start(GTK_BOX(ticket_page), ticket_container, TRUE, TRUE, 0);

    GtkWidget *ticket_info = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    g_ticket_date_label = gtk_label_new("Fecha: ");
    g_ticket_total_label = gtk_label_new("Total: $ 0.00");
    gtk_style_context_add_class(gtk_widget_get_style_context(g_ticket_total_label), "total-label");
    gtk_box_pack_start(GTK_BOX(ticket_info), g_ticket_date_label, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(ticket_info), g_ticket_total_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ticket_container), ticket_info, FALSE, FALSE, 0);

    GtkWidget *ticket_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ticket_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(ticket_sw, TRUE);
    g_ticket_list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_ticket_list_box), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(ticket_sw), g_ticket_list_box);
    gtk_box_pack_start(GTK_BOX(ticket_container), ticket_sw, TRUE, TRUE, 0);

    GtkWidget *ticket_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_ticket_save_button = gtk_button_new_with_label("Guardar Ticket");
    gtk_style_context_add_class(gtk_widget_get_style_context(g_ticket_save_button), "suggested-action");
    g_signal_connect(g_ticket_save_button, "clicked", G_CALLBACK(on_save_ticket_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(ticket_actions), g_ticket_save_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ticket_container), ticket_actions, FALSE, FALSE, 0);

    gtk_stack_add_named(GTK_STACK(main_stack), ticket_page, "ticket_view");

    /* Admin view */
    GtkWidget *admin_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(admin_page, 10);
    gtk_widget_set_margin_end(admin_page, 10);
    gtk_widget_set_margin_top(admin_page, 10);
    gtk_widget_set_margin_bottom(admin_page, 10);

    GtkWidget *admin_back = gtk_button_new_with_label("← Volver a la Tienda");
    g_signal_connect(admin_back, "clicked", G_CALLBACK(on_back_to_shop_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(admin_page), admin_back, FALSE, FALSE, 0);

    g_admin_status_label = gtk_label_new("Inicia sesión como administrador para gestionar el inventario.");
    gtk_box_pack_start(GTK_BOX(admin_page), g_admin_status_label, FALSE, FALSE, 4);

    GtkWidget *admin_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(admin_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(admin_sw, TRUE);
    g_admin_list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_admin_list_box), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(admin_sw), g_admin_list_box);
    gtk_box_pack_start(GTK_BOX(admin_page), admin_sw, TRUE, TRUE, 0);

    gtk_stack_add_named(GTK_STACK(main_stack), admin_page, "admin_view");

    gtk_widget_show_all(window);
    gtk_widget_grab_default(login_button);
    gtk_widget_hide(g_login_error_label);
    gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "login_view");
    gtk_widget_grab_focus(g_login_username_entry);
    gtk_main();

    if (server_socket >= 0) close(server_socket);
    g_free(g_last_ticket_raw);
    g_free(g_current_user);
    g_free(g_user_role);
    return 0;
}

/* --- Callbacks --- */

static void cleanup_and_exit(GtkWidget *widget, gpointer user_data) {
    gtk_main_quit();
}

static void on_start_shopping_clicked(GtkButton *button, gpointer user_data) {
    if (!g_current_user) {
        gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "login_view");
        gtk_widget_grab_focus(g_login_username_entry);
        return;
    }
    gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "brands_view");
}

static void on_back_to_brands_clicked(GtkButton *button, gpointer user_data) {
    gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "brands_view");
}

static void add_btn_cb(GtkButton *btn, gpointer unused) {
    const char *m = g_object_get_data(G_OBJECT(btn), "model-name");
    if (m) on_add_to_cart_clicked(btn, m);
}

/* Mostrar modelos (modelo|specs|precio|imagen) */
static void on_brand_clicked(GtkButton *button, GtkWidget *model_container) {
    const char *brand = g_object_get_data(G_OBJECT(button), "brand-name");
    if (!brand || !*brand) brand = gtk_button_get_label(button);
    if (!brand || !*brand) return;
    char command[256];
    snprintf(command, sizeof(command), "GET_MODELS:%s", brand);
    char *response = send_command(command);
    if (!response) return;

    clear_container(model_container);
    char *resp_copy = g_strdup(response);
    char *saveptr;
    char *line = strtok_r(resp_copy, "\n", &saveptr);
    while (line && *line) {
        char *sp;
        char *model_name = strtok_r(line, "|", &sp);
        char *specs      = strtok_r(NULL, "|", &sp);
        char *price      = strtok_r(NULL, "|", &sp);
        char *image_path = strtok_r(NULL, "|", &sp);
        if (model_name && specs && price && image_path) {
            GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
            gtk_widget_set_margin_start(card, 10);
            gtk_widget_set_margin_end(card, 10);
            gtk_widget_set_margin_top(card, 6);
            gtk_widget_set_margin_bottom(card, 6);
            gtk_widget_set_size_request(card, 220, 360);
            gtk_style_context_add_class(gtk_widget_get_style_context(card), "model-card");

            /* Caja de imagen */
            GtkWidget *img_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            gtk_style_context_add_class(gtk_widget_get_style_context(img_box), "model-card-image-box");
            gtk_widget_set_halign(img_box, GTK_ALIGN_CENTER);

            GdkPixbuf *px = scale_pixbuf(image_path, 160, 160);
            GtkWidget *img = gtk_image_new_from_pixbuf(px);
            if (px) g_object_unref(px);
            gtk_box_pack_start(GTK_BOX(img_box), img, FALSE, FALSE, 0);

            /* Caja de contenido */
            GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
            gtk_style_context_add_class(gtk_widget_get_style_context(content_box), "model-card-content");

            GtkWidget *name_label = gtk_label_new(NULL);
            char name_markup[256];
            snprintf(name_markup, sizeof(name_markup), "<span weight='bold'>%s</span>", model_name);
            gtk_label_set_markup(GTK_LABEL(name_label), name_markup);
            gtk_label_set_line_wrap(GTK_LABEL(name_label), TRUE);
            gtk_widget_set_halign(name_label, GTK_ALIGN_CENTER);
            gtk_style_context_add_class(gtk_widget_get_style_context(name_label), "model-card-title");

            GtkWidget *specs_label = gtk_label_new(specs);
            gtk_label_set_line_wrap(GTK_LABEL(specs_label), TRUE);
            gtk_widget_set_size_request(specs_label, 200, -1);
            gtk_widget_set_halign(specs_label, GTK_ALIGN_CENTER);
            gtk_style_context_add_class(gtk_widget_get_style_context(specs_label), "model-card-specs");

            GtkWidget *price_label = gtk_label_new(NULL);
            char price_markup[128];
            snprintf(price_markup, sizeof(price_markup), "<span weight='bold'>$ %s</span>", price);
            gtk_label_set_markup(GTK_LABEL(price_label), price_markup);
            gtk_widget_set_halign(price_label, GTK_ALIGN_CENTER);
            gtk_style_context_add_class(gtk_widget_get_style_context(price_label), "model-card-price");

            GtkWidget *add_button = gtk_button_new_with_label("Añadir al carrito");
            g_object_set_data_full(G_OBJECT(add_button), "model-name", g_strdup(model_name), g_free);
            gtk_style_context_add_class(gtk_widget_get_style_context(add_button), "suggested-action");
            g_signal_connect(add_button, "clicked", G_CALLBACK(add_btn_cb), NULL);

            gtk_box_pack_start(GTK_BOX(content_box), name_label, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(content_box), specs_label, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(content_box), price_label, FALSE, FALSE, 4);
            gtk_box_pack_end(GTK_BOX(content_box), add_button, FALSE, FALSE, 0);

            gtk_box_pack_start(GTK_BOX(card), img_box, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(card), content_box, TRUE, TRUE, 0);

            gtk_flow_box_insert(GTK_FLOW_BOX(model_container), card, -1);
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    g_free(resp_copy);
    gtk_widget_show_all(model_container);
    gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "models_view");
}

static void on_add_to_cart_clicked(GtkButton *button, const char *model_name) {
    char command[256];
    snprintf(command, sizeof(command), "ADD_TO_CART:%s", model_name);
    char *resp = send_command(command);
    if (!resp || strncmp(resp, "OK", 2) != 0) {
        GtkWidget *dialog = gtk_message_dialog_new(
            NULL, GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "No se pudo añadir al carrito.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }
    GtkWidget *dialog = gtk_message_dialog_new(
        NULL, GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "¡%s añadido al carrito!", model_name);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

/* Calcular total carrito y poblar lista */
static void on_view_cart_clicked(GtkButton *button, gpointer user_data) {
    if (!g_current_user) {
        gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "login_view");
        gtk_widget_grab_focus(g_login_username_entry);
        return;
    }
    clear_container(g_cart_list_box);
    double total = 0.0;
    char *response = send_command("GET_CART_ITEMS");
    if (!response) {
        gtk_label_set_text(GTK_LABEL(g_cart_total_label), "Total carrito: $ 0.00");
        gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "cart_view");
        return;
    }
    if (strncmp(response, "EMPTY", 5) == 0) {
        GtkWidget *empty_label = gtk_label_new("Tu carrito está vacío.");
        gtk_list_box_insert(GTK_LIST_BOX(g_cart_list_box), empty_label, -1);
        gtk_widget_show(empty_label);
        gtk_label_set_text(GTK_LABEL(g_cart_total_label), "Total carrito: $ 0.00");
        gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "cart_view");
        return;
    }
    char *copy = g_strdup(response);
    char *saveptr;
    char *line = strtok_r(copy, "\n", &saveptr);
    while (line) {
        char *sp;
        char *modelo = strtok_r(line, "|", &sp);
        char *marca  = strtok_r(NULL, "|", &sp);
        char *specs  = strtok_r(NULL, "|", &sp);
        char *precio = strtok_r(NULL, "|", &sp);
        char *imagen = strtok_r(NULL, "|", &sp);
        if (modelo && marca && specs && precio && imagen) {
            double p = atof(precio);
            total += p;

            GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 15);
            gtk_widget_set_margin_top(row_box, 10);
            gtk_widget_set_margin_bottom(row_box, 10);
            gtk_widget_set_margin_start(row_box, 10);
            gtk_widget_set_margin_end(row_box, 10);
            gtk_style_context_add_class(gtk_widget_get_style_context(row_box), "cart-row");

            GdkPixbuf *pixbuf = scale_pixbuf(imagen, 100, 100);
            GtkWidget *image_widget = gtk_image_new_from_pixbuf(pixbuf);
            if (pixbuf) g_object_unref(pixbuf);

            GtkWidget *details_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
            GtkWidget *model_label = gtk_label_new(NULL);
            char mk[256];
            snprintf(mk, sizeof(mk), "<span weight='bold' size='large'>%s</span>", modelo);
            gtk_label_set_markup(GTK_LABEL(model_label), mk);
            gtk_widget_set_halign(model_label, GTK_ALIGN_START);

            GtkWidget *brand_label = gtk_label_new(marca);
            gtk_widget_set_halign(brand_label, GTK_ALIGN_START);

            GtkWidget *specs_label = gtk_label_new(specs);
            gtk_label_set_line_wrap(GTK_LABEL(specs_label), TRUE);
            gtk_widget_set_halign(specs_label, GTK_ALIGN_START);

            GtkWidget *price_label = gtk_label_new(NULL);
            snprintf(mk, sizeof(mk), "<span weight='bold'>$ %s</span>", precio);
            gtk_label_set_markup(GTK_LABEL(price_label), mk);
            gtk_widget_set_halign(price_label, GTK_ALIGN_START);

            gtk_box_pack_start(GTK_BOX(details_box), model_label, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(details_box), brand_label, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(details_box), specs_label, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(details_box), price_label, FALSE, FALSE, 5);

            gtk_box_pack_start(GTK_BOX(row_box), image_widget, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(row_box), details_box, TRUE, TRUE, 0);

            g_object_set_data_full(G_OBJECT(row_box), "data-modelo", g_strdup(modelo), g_free);
            g_object_set_data_full(G_OBJECT(row_box), "data-marca",  g_strdup(marca), g_free);
            g_object_set_data_full(G_OBJECT(row_box), "data-specs",  g_strdup(specs), g_free);

            gtk_list_box_insert(GTK_LIST_BOX(g_cart_list_box), row_box, -1);
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    g_free(copy);

    char total_text[128];
    snprintf(total_text, sizeof(total_text), "Total carrito: $ %.2f", total);
    gtk_label_set_text(GTK_LABEL(g_cart_total_label), total_text);

    gtk_widget_show_all(g_cart_list_box);
    gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "cart_view");
}

static void on_back_to_shop_clicked(GtkButton *button, gpointer user_data) {
    gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "brands_view");
}

static void on_cart_filter_changed(GtkEditable *editable, gpointer user_data) {
    CartFilterData *data = &g_cart_filter_data;
    char *f_modelo = str_to_lower(gtk_entry_get_text(GTK_ENTRY(data->filter_modelo)));
    char *f_marca  = str_to_lower(gtk_entry_get_text(GTK_ENTRY(data->filter_marca)));
    char *f_proc   = str_to_lower(gtk_entry_get_text(GTK_ENTRY(data->filter_procesador)));

    GList *rows = gtk_container_get_children(GTK_CONTAINER(data->cart_list_box));
    for (GList *iter = rows; iter != NULL; iter = g_list_next(iter)) {
        GtkWidget *row = GTK_WIDGET(iter->data);
        if (!GTK_IS_LIST_BOX_ROW(row)) continue;
        GtkWidget *row_box = gtk_bin_get_child(GTK_BIN(row));
        if (!GTK_IS_BOX(row_box)) continue;
        char *d_modelo = str_to_lower(g_object_get_data(G_OBJECT(row_box), "data-modelo"));
        char *d_marca  = str_to_lower(g_object_get_data(G_OBJECT(row_box), "data-marca"));
        char *d_specs  = str_to_lower(g_object_get_data(G_OBJECT(row_box), "data-specs"));
        gboolean visible = TRUE;
        if (f_modelo[0] && (!d_modelo || !strstr(d_modelo, f_modelo))) visible = FALSE;
        if (f_marca[0]  && (!d_marca  || !strstr(d_marca,  f_marca)))  visible = FALSE;
        if (f_proc[0]   && (!d_specs  || !strstr(d_specs,  f_proc)))   visible = FALSE;
        gtk_widget_set_visible(row, visible);
        g_free(d_modelo); g_free(d_marca); g_free(d_specs);
    }
    g_list_free(rows);
    g_free(f_modelo); g_free(f_marca); g_free(f_proc);
}

/* Pago */
static void payment_radio_toggled(GtkToggleButton *toggle, gpointer user_data) {
    if (!gtk_toggle_button_get_active(toggle)) return;
    GtkStack *stack = GTK_STACK(user_data);
    const char *label = gtk_button_get_label(GTK_BUTTON(toggle));
    if (g_strcmp0(label, "PayPal") == 0)
        gtk_stack_set_visible_child_name(stack, "PayPal");
    else if (g_strcmp0(label, "Tarjeta (TDC/TDD)") == 0)
        gtk_stack_set_visible_child_name(stack, "TDC/TDD");
    else if (g_strcmp0(label, "Depósito en OXXO") == 0)
        gtk_stack_set_visible_child_name(stack, "Depósito en OXXO");
}

static void on_card_number_changed(GtkEditable *editable, gpointer user_data) {
    const char *text = gtk_entry_get_text(GTK_ENTRY(editable));
    char digits[32] = "";
    int di = 0;
    for (int i = 0; text[i] && di < 19; ++i)
        if (isdigit((unsigned char)text[i])) digits[di++] = text[i];
    digits[di] = 0;
    char formatted[48] = "";
    int fi = 0;
    for (int i = 0; i < di; ++i) {
        if (i && i % 4 == 0 && fi < (int)sizeof(formatted)-1) formatted[fi++] = ' ';
        if (fi < (int)sizeof(formatted)-1) formatted[fi++] = digits[i];
    }
    formatted[fi] = 0;
    if (strcmp(formatted, text) != 0) {
        gint pos = gtk_editable_get_position(editable);
        gtk_entry_set_text(GTK_ENTRY(editable), formatted);
        gtk_editable_set_position(editable, pos);
    }
}

static void on_card_exp_changed(GtkEditable *editable, gpointer user_data) {
    const char *text = gtk_entry_get_text(GTK_ENTRY(editable));
    char digits[8] = "";
    int di = 0;
    for (int i = 0; text[i] && di < 4; ++i)
        if (isdigit((unsigned char)text[i])) digits[di++] = text[i];
    digits[di] = 0;
    char formatted[8] = "";
    if (di >= 1) {
        formatted[0] = digits[0];
        if (di >= 2) {
            formatted[1] = digits[1];
            formatted[2] = '/';
            if (di >= 3) formatted[3] = digits[2];
            if (di >= 4) formatted[4] = digits[3];
        }
    }
    int flen = strlen(formatted);
    formatted[flen] = 0;
    if (strcmp(formatted, text) != 0) {
        gint pos = gtk_editable_get_position(editable);
        gtk_entry_set_text(GTK_ENTRY(editable), formatted);
        gtk_editable_set_position(editable, pos);
    }
}

static void on_card_cvv_changed(GtkEditable *editable, gpointer user_data) {
    const char *text = gtk_entry_get_text(GTK_ENTRY(editable));
    char digits[5] = "";
    int di = 0;
    for (int i = 0; text[i] && di < 4; ++i)
        if (isdigit((unsigned char)text[i])) digits[di++] = text[i];
    digits[di] = 0;
    if (strcmp(digits, text) != 0) {
        gint pos = gtk_editable_get_position(editable);
        gtk_entry_set_text(GTK_ENTRY(editable), digits);
        gtk_editable_set_position(editable, pos);
    }
}

static void on_checkout_clicked(GtkButton *button, gpointer user_data) {
    if (!g_current_user) {
        gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "login_view");
        gtk_widget_grab_focus(g_login_username_entry);
        return;
    }
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Pago", NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancelar", GTK_RESPONSE_CANCEL,
        "_Pagar", GTK_RESPONSE_OK,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 520, 360);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(content), outer, TRUE, TRUE, 8);

    GtkWidget *payment_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(payment_box), "payment-box");
    gtk_box_pack_start(GTK_BOX(outer), payment_box, TRUE, TRUE, 0);

    GtkWidget *radio_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *rb1 = gtk_radio_button_new_with_label(NULL, "PayPal");
    GtkWidget *rb2 = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(rb1), "Tarjeta (TDC/TDD)");
    GtkWidget *rb3 = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(rb1), "Depósito en OXXO");
    gtk_box_pack_start(GTK_BOX(radio_box), rb1, FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(radio_box), rb2, FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(radio_box), rb3, FALSE, FALSE, 6);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rb1), TRUE);
    gtk_box_pack_start(GTK_BOX(payment_box), radio_box, FALSE, FALSE, 6);

    GtkWidget *stack = gtk_stack_new();
    gtk_widget_set_hexpand(stack, TRUE);
    gtk_widget_set_vexpand(stack, TRUE);

    GtkWidget *paypal_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *paypal_title = gtk_label_new("Paga con tu cuenta PayPal");
    gtk_style_context_add_class(gtk_widget_get_style_context(paypal_title), "subtitle-label");
    GtkWidget *paypal_label = gtk_label_new("Correo PayPal:");
    GtkWidget *paypal_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(paypal_entry), "email@ejemplo.com");
    gtk_box_pack_start(GTK_BOX(paypal_box), paypal_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(paypal_box), paypal_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(paypal_box), paypal_entry, FALSE, FALSE, 0);

    GtkWidget *card_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *card_title = gtk_label_new("Paga con tarjeta de crédito o débito");
    gtk_style_context_add_class(gtk_widget_get_style_context(card_title), "subtitle-label");
    GtkWidget *card_number = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(card_number), "Número de tarjeta");
    GtkWidget *card_name = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(card_name), "Nombre del titular");
    GtkWidget *card_exp = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(card_exp), "MM/AA");
    GtkWidget *card_cvv = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(card_cvv), "CVV");
    gtk_box_pack_start(GTK_BOX(card_box), card_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card_box), card_number, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card_box), card_name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card_box), card_exp, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card_box), card_cvv, FALSE, FALSE, 0);
    g_signal_connect(card_number, "changed", G_CALLBACK(on_card_number_changed), NULL);
    g_signal_connect(card_exp, "changed", G_CALLBACK(on_card_exp_changed), NULL);
    g_signal_connect(card_cvv, "changed", G_CALLBACK(on_card_cvv_changed), NULL);

    GtkWidget *oxxo_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *oxxo_title = gtk_label_new("Paga en efectivo en cualquier OXXO");
    gtk_style_context_add_class(gtk_widget_get_style_context(oxxo_title), "subtitle-label");
    GtkWidget *oxxo_label = gtk_label_new("Se generará un folio al confirmar tu compra.");
    gtk_box_pack_start(GTK_BOX(oxxo_box), oxxo_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(oxxo_box), oxxo_label, FALSE, FALSE, 0);

    gtk_stack_add_named(GTK_STACK(stack), paypal_box, "PayPal");
    gtk_stack_add_named(GTK_STACK(stack), card_box, "TDC/TDD");
    gtk_stack_add_named(GTK_STACK(stack), oxxo_box, "Depósito en OXXO");
    gtk_box_pack_start(GTK_BOX(payment_box), stack, TRUE, TRUE, 0);

    g_signal_connect(rb1, "toggled", G_CALLBACK(payment_radio_toggled), stack);
    g_signal_connect(rb2, "toggled", G_CALLBACK(payment_radio_toggled), stack);
    g_signal_connect(rb3, "toggled", G_CALLBACK(payment_radio_toggled), stack);

    GtkWidget **widgets = g_new0(GtkWidget*, 6);
    widgets[0] = radio_box;
    widgets[1] = paypal_entry;
    widgets[2] = card_number;
    widgets[3] = card_exp;
    widgets[4] = card_cvv;
    widgets[5] = card_name;

    g_signal_connect(dialog, "response", G_CALLBACK(on_checkout_submit), widgets);
    gtk_widget_show_all(dialog);
}

/* Checkout submit */
static void on_checkout_submit(GtkDialog *dialog, gint response_id, gpointer user_data) {
    GtkWidget **widgets = (GtkWidget**)user_data;
    if (response_id != GTK_RESPONSE_OK) {
        g_free(widgets);
        gtk_widget_destroy(GTK_WIDGET(dialog));
        return;
    }
    GtkWidget *radio_box = widgets[0];
    const char *method = NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(radio_box));
    for (GList *it = children; it != NULL; it = g_list_next(it)) {
        if (GTK_IS_RADIO_BUTTON(it->data) &&
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(it->data))) {
            method = gtk_button_get_label(GTK_BUTTON(it->data));
            break;
        }
    }
    g_list_free(children);
    if (!method) method = "PayPal";

    gboolean valid = TRUE;
    char errmsg[256] = "";
    if (g_strcmp0(method, "PayPal") == 0) {
        const char *email = gtk_entry_get_text(GTK_ENTRY(widgets[1]));
        if (strlen(email) < 5 || !strchr(email, '@'))
            { valid = FALSE; snprintf(errmsg, sizeof(errmsg), "Correo PayPal inválido."); }
    } else if (g_strcmp0(method, "Tarjeta (TDC/TDD)") == 0) {
        const char *card = gtk_entry_get_text(GTK_ENTRY(widgets[2]));
        const char *exp  = gtk_entry_get_text(GTK_ENTRY(widgets[3]));
        const char *cvv  = gtk_entry_get_text(GTK_ENTRY(widgets[4]));
        const char *name = gtk_entry_get_text(GTK_ENTRY(widgets[5]));
        int digits_card = 0;
        for (int i = 0; card[i]; ++i) if (isdigit((unsigned char)card[i])) digits_card++;
        if (digits_card < 13 || digits_card > 19)
            { valid = FALSE; snprintf(errmsg, sizeof(errmsg), "Número de tarjeta inválido."); }
        else if (strlen(cvv) < 3 || strlen(cvv) > 4)
            { valid = FALSE; snprintf(errmsg, sizeof(errmsg), "CVV inválido."); }
        else if (strlen(exp) < 4 || exp[2] != '/')
            { valid = FALSE; snprintf(errmsg, sizeof(errmsg), "Formato fecha MM/AA inválido."); }
        else if (strlen(name) < 2)
            { valid = FALSE; snprintf(errmsg, sizeof(errmsg), "Nombre del titular requerido."); }
    } else if (g_strcmp0(method, "Depósito en OXXO") == 0) {
        /* Sin validación extra */
    } else {
        valid = FALSE; snprintf(errmsg, sizeof(errmsg), "Método desconocido.");
    }

    if (!valid) {
        GtkWidget *errd = gtk_message_dialog_new(GTK_WINDOW(dialog), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", errmsg);
        gtk_dialog_run(GTK_DIALOG(errd));
        gtk_widget_destroy(errd);
        return;
    }

    const char *server_method =
        (g_strcmp0(method, "Depósito en OXXO") == 0) ? "OXXO" :
        (g_strcmp0(method, "Tarjeta (TDC/TDD)") == 0 ? "TDC/TDD" : "PayPal");

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "CHECKOUT:%s", server_method);
    char *resp = send_command(cmd);
    if (!resp) {
        g_free(widgets);
        gtk_widget_destroy(GTK_WIDGET(dialog));
        return;
    }

    g_free(g_last_ticket_raw);
    g_last_ticket_raw = g_strdup(resp);

    char *copy = g_strdup(resp);
    char *saveptr;
    char *line = strtok_r(copy, "\n", &saveptr);
    if (!line || strncmp(line, "OK|", 3) != 0) {
        show_net_error_and_keep_ui("Error en el pago.");
        g_free(copy);
        g_free(widgets);
        gtk_widget_destroy(GTK_WIDGET(dialog));
        return;
    }
    char *sp;
    strtok_r(line, "|", &sp);
    char *fecha = strtok_r(NULL, "|", &sp);
    char *total = strtok_r(NULL, "|", &sp);

    clear_container(g_ticket_list_box);
    g_folio_row = NULL;

    char datebuf[128]; snprintf(datebuf, sizeof(datebuf), "Fecha: %s", fecha ? fecha : "");
    gtk_label_set_text(GTK_LABEL(g_ticket_date_label), datebuf);
    char totalbuf[128]; snprintf(totalbuf, sizeof(totalbuf), "Total: $ %s", total ? total : "0.00");
    gtk_label_set_text(GTK_LABEL(g_ticket_total_label), totalbuf);

    line = strtok_r(NULL, "\n", &saveptr);
    while (line) {
        char *isp;
        char *modelo = strtok_r(line, "|", &isp);
        char *marca  = strtok_r(NULL, "|", &isp);
        char *specs  = strtok_r(NULL, "|", &isp);
        char *precio = strtok_r(NULL, "|", &isp);
        char *imagen = strtok_r(NULL, "|", &isp);
        if (modelo && marca && specs && precio && imagen) {
            GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            gtk_style_context_add_class(gtk_widget_get_style_context(row_box), "ticket-row");
            GdkPixbuf *px = scale_pixbuf(imagen, 64, 64);
            GtkWidget *img = gtk_image_new_from_pixbuf(px);
            if (px) g_object_unref(px);
            GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

            GtkWidget *l1 = gtk_label_new(NULL);
            char m1[256]; snprintf(m1, sizeof(m1), "<span weight='bold'>%s</span> - %s", modelo, marca);
            gtk_label_set_markup(GTK_LABEL(l1), m1);

            GtkWidget *l2 = gtk_label_new(specs);
            gtk_label_set_line_wrap(GTK_LABEL(l2), TRUE);

            GtkWidget *l3 = gtk_label_new(NULL);
            char m3[64]; snprintf(m3, sizeof(m3), "$ %s", precio);
            gtk_label_set_markup(GTK_LABEL(l3), m3);

            gtk_box_pack_start(GTK_BOX(v), l1, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(v), l2, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(v), l3, FALSE, FALSE, 0);

            gtk_box_pack_start(GTK_BOX(row_box), img, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(row_box), v, TRUE, TRUE, 0);

            gtk_list_box_insert(GTK_LIST_BOX(g_ticket_list_box), row_box, -1);
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    if (g_strcmp0(method, "Depósito en OXXO") == 0) {
        char folio[64];
        srand((unsigned int)time(NULL));
        snprintf(folio, sizeof(folio), "OXXO-%09d", rand() % 1000000000);
        char nf[160];
        snprintf(nf, sizeof(nf), "Folio de depósito: %s (preséntalo en OXXO)", folio);

        GtkWidget *folio_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_style_context_add_class(gtk_widget_get_style_context(folio_hbox), "folio-row");
        GtkWidget *folio_label = gtk_label_new(nf);
        gtk_label_set_selectable(GTK_LABEL(folio_label), TRUE);
        GtkWidget *copy_btn = gtk_button_new_with_label("Copiar folio");
        g_object_set_data_full(G_OBJECT(copy_btn), "folio-text", g_strdup(folio), g_free);
        g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_copy_folio_clicked), folio_label);

        gtk_box_pack_start(GTK_BOX(folio_hbox), folio_label, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(folio_hbox), copy_btn, FALSE, FALSE, 0);

        gtk_list_box_insert(GTK_LIST_BOX(g_ticket_list_box), folio_hbox, -1);
        g_folio_row = folio_hbox;
    }

    g_free(copy);
    g_free(widgets);
    gtk_widget_destroy(GTK_WIDGET(dialog));
    gtk_widget_show_all(g_ticket_list_box);
    gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "ticket_view");
}

/* Copiar folio */
static void on_copy_folio_clicked(GtkButton *button, gpointer user_data) {
    GtkWidget *folio_label = GTK_WIDGET(user_data);
    const char *folio_full = gtk_label_get_text(GTK_LABEL(folio_label));
    const char *start = strstr(folio_full, "OXXO-");
    const char *folio_to_copy = start ? start : folio_full;
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(cb, folio_to_copy, -1);

    GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "Folio copiado al portapapeles.");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

/* Guardar ticket */
static void on_save_ticket_clicked(GtkButton *button, gpointer user_data) {
    if (!g_last_ticket_raw) {
        GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "No hay ticket para guardar.");
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        return;
    }

    GtkWidget *chooser = gtk_file_chooser_dialog_new("Guardar Ticket",
        NULL,
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancelar", GTK_RESPONSE_CANCEL,
        "_Guardar", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser), "ticket.csv");

    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        FILE *f = fopen(filename, "w");
        if (!f) {
            GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                "No se pudo abrir el archivo para escribir.");
            gtk_dialog_run(GTK_DIALOG(d));
            gtk_widget_destroy(d);
            g_free(filename);
            gtk_widget_destroy(chooser);
            return;
        }
        fprintf(f, "Modelo,Marca,Specs,Precio,Imagen\n");

        char *copy = g_strdup(g_last_ticket_raw);
        char *saveptr;
        char *line = strtok_r(copy, "\n", &saveptr);
        if (line && strncmp(line, "OK|", 3) == 0) {
            line = strtok_r(NULL, "\n", &saveptr);
        }
        while (line) {
            char *sp;
            char *modelo = strtok_r(line, "|", &sp);
            char *marca  = strtok_r(NULL, "|", &sp);
            char *specs  = strtok_r(NULL, "|", &sp);
            char *precio = strtok_r(NULL, "|", &sp);
            char *imagen = strtok_r(NULL, "|", &sp);
            if (modelo && marca && specs && precio && imagen) {
                fprintf(f, "\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"\n",
                        modelo, marca, specs, precio, imagen);
            }
            line = strtok_r(NULL, "\n", &saveptr);
        }
        g_free(copy);
        fclose(f);

        GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
            GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
            "Ticket guardado en:\n%s", filename);
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        g_free(filename);
    }

    gtk_widget_destroy(chooser);
}

static gboolean attempt_login(const char *username, const char *password, char **role_out, char **error_out) {
    if (role_out) *role_out = NULL;
    if (error_out) *error_out = NULL;
    if (!username || !*username || !password || !*password) {
        if (error_out) *error_out = g_strdup("Ingresa usuario y contraseña.");
        return FALSE;
    }
    if (strchr(username, '|') || strchr(password, '|')) {
        if (error_out) *error_out = g_strdup("El caracter '|' no está permitido.");
        return FALSE;
    }
    if (strpbrk(username, "\r\n") || strpbrk(password, "\r\n")) {
        if (error_out) *error_out = g_strdup("No uses saltos de línea en las credenciales.");
        return FALSE;
    }

    char command[512];
    snprintf(command, sizeof(command), "LOGIN:%s|%s", username, password);
    char *resp = send_command(command);
    if (!resp) {
        if (error_out) *error_out = g_strdup("No se pudo contactar al servidor.");
        return FALSE;
    }

    if (g_str_has_prefix(resp, "OK")) {
        const char *role = NULL;
        if (resp[2] == '|') {
            role = resp + 3;
        } else if (resp[2] == '\0' || resp[2] == '\n') {
            role = "cliente";
        }
        if (role) {
            char *newline = strchr(role, '\n');
            if (newline) *newline = '\0';
            if (role_out) *role_out = g_strdup(role);
        }
        return TRUE;
    }

    if (error_out) {
        if (g_str_has_prefix(resp, "ERROR|"))
            *error_out = g_strdup(resp + 6);
        else
            *error_out = g_strdup("Credenciales incorrectas.");
    }
    return FALSE;
}

static gboolean attempt_register(const char *username, const char *password, char **error_out) {
    if (error_out) *error_out = NULL;
    if (!username || !*username || !password || !*password) {
        if (error_out) *error_out = g_strdup("Ingresa usuario y contraseña.");
        return FALSE;
    }
    if (strlen(username) < 3) {
        if (error_out) *error_out = g_strdup("El usuario debe tener al menos 3 caracteres.");
        return FALSE;
    }
    if (strlen(password) < 4) {
        if (error_out) *error_out = g_strdup("La contraseña debe tener al menos 4 caracteres.");
        return FALSE;
    }
    if (strchr(username, '|') || strchr(password, '|')) {
        if (error_out) *error_out = g_strdup("El caracter '|' no está permitido.");
        return FALSE;
    }
    if (strpbrk(username, "\r\n") || strpbrk(password, "\r\n")) {
        if (error_out) *error_out = g_strdup("No uses saltos de línea.");
        return FALSE;
    }

    char command[512];
    snprintf(command, sizeof(command), "REGISTER:%s|%s", username, password);
    char *resp = send_command(command);
    if (!resp) {
        if (error_out) *error_out = g_strdup("No se pudo contactar al servidor.");
        return FALSE;
    }

    if (g_str_has_prefix(resp, "OK")) {
        return TRUE;
    }

    if (g_str_has_prefix(resp, "ERROR|")) {
        char *msg = resp + 6;
        char *newline = strchr(msg, '\n');
        if (newline) *newline = '\0';
        if (error_out) *error_out = g_strdup(msg);
    } else if (error_out) {
        *error_out = g_strdup("Registro rechazado.");
    }
    return FALSE;
}

static void on_login_clicked(GtkButton *button, gpointer user_data) {
    const char *username = gtk_entry_get_text(GTK_ENTRY(g_login_username_entry));
    const char *password = gtk_entry_get_text(GTK_ENTRY(g_login_password_entry));
    char *error_msg = NULL;
    char *role = NULL;

    gtk_widget_set_visible(g_admin_button, FALSE);

    gboolean ok = attempt_login(username, password, &role, &error_msg);

    if (!ok) {
        g_free(role);
        if (error_msg) {
            gtk_label_set_text(GTK_LABEL(g_login_error_label), error_msg);
            g_free(error_msg);
        } else {
            gtk_label_set_text(GTK_LABEL(g_login_error_label), "Credenciales incorrectas.");
        }
        gtk_widget_show(g_login_error_label);
        gtk_entry_set_text(GTK_ENTRY(g_login_password_entry), "");
        if (!username || !*username)
            gtk_widget_grab_focus(g_login_username_entry);
        else
            gtk_widget_grab_focus(g_login_password_entry);
        return;
    }

    gtk_widget_hide(g_login_error_label);
    gtk_entry_set_text(GTK_ENTRY(g_login_username_entry), "");
    gtk_entry_set_text(GTK_ENTRY(g_login_password_entry), "");

    g_free(g_current_user);
    g_current_user = g_strdup(username);
    g_free(g_user_role);
    g_user_role = role ? role : g_strdup("cliente");

    gboolean is_admin = (g_user_role && g_strcmp0(g_user_role, "admin") == 0);
    gtk_widget_set_visible(g_admin_button, is_admin);
    gtk_widget_set_sensitive(g_cart_button, TRUE);
    if (g_admin_status_label) {
        gtk_label_set_text(GTK_LABEL(g_admin_status_label),
                           is_admin ? "Selecciona un modelo para eliminarlo del inventario." :
                                      "Inicia sesión como administrador para gestionar el inventario.");
    }

    char *welcome_markup = g_markup_printf_escaped("<span size='xx-large'>Bienvenido %s</span>", g_current_user);
    gtk_label_set_markup(GTK_LABEL(g_welcome_label), welcome_markup);
    g_free(welcome_markup);

    gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "welcome_view");
}

static void on_register_clicked(GtkButton *button, gpointer user_data) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Crear cuenta", NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancelar", GTK_RESPONSE_CANCEL,
        "_Crear", GTK_RESPONSE_OK,
        NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(content), box, TRUE, TRUE, 8);

    GtkWidget *user_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(user_entry), "Usuario");
    gtk_entry_set_activates_default(GTK_ENTRY(user_entry), TRUE);
    GtkWidget *pass_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(pass_entry), "Contraseña");
    gtk_entry_set_visibility(GTK_ENTRY(pass_entry), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(pass_entry), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_activates_default(GTK_ENTRY(pass_entry), TRUE);
    GtkWidget *confirm_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(confirm_entry), "Confirmar contraseña");
    gtk_entry_set_visibility(GTK_ENTRY(confirm_entry), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(confirm_entry), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_activates_default(GTK_ENTRY(confirm_entry), TRUE);

    GtkWidget *error_label = gtk_label_new(NULL);
    gtk_widget_set_halign(error_label, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(error_label), "error-text");
    gtk_widget_hide(error_label);

    gtk_box_pack_start(GTK_BOX(box), user_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), pass_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), confirm_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), error_label, FALSE, FALSE, 0);

    gtk_widget_show_all(dialog);
    gtk_widget_hide(error_label);

    gboolean created = FALSE;
    while (TRUE) {
        gint resp = gtk_dialog_run(GTK_DIALOG(dialog));
        if (resp != GTK_RESPONSE_OK) break;

        const char *new_user = gtk_entry_get_text(GTK_ENTRY(user_entry));
        const char *new_pass = gtk_entry_get_text(GTK_ENTRY(pass_entry));
        const char *new_confirm = gtk_entry_get_text(GTK_ENTRY(confirm_entry));

        if (g_strcmp0(new_pass, new_confirm) != 0) {
            gtk_label_set_text(GTK_LABEL(error_label), "Las contraseñas no coinciden.");
            gtk_widget_show(error_label);
            gtk_entry_set_text(GTK_ENTRY(pass_entry), "");
            gtk_entry_set_text(GTK_ENTRY(confirm_entry), "");
            gtk_widget_grab_focus(confirm_entry);
            continue;
        }

        char *error_msg = NULL;
        gboolean ok = attempt_register(new_user, new_pass, &error_msg);
        if (!ok) {
            gtk_label_set_text(GTK_LABEL(error_label), error_msg ? error_msg : "Registro rechazado.");
            gtk_widget_show(error_label);
            g_free(error_msg);
            gtk_entry_set_text(GTK_ENTRY(pass_entry), "");
            gtk_entry_set_text(GTK_ENTRY(confirm_entry), "");
            gtk_widget_grab_focus(pass_entry);
            continue;
        }

        created = TRUE;
        gtk_entry_set_text(GTK_ENTRY(g_login_username_entry), new_user);
        gtk_entry_set_text(GTK_ENTRY(g_login_password_entry), "");
        gtk_widget_hide(error_label);
        break;
    }

    gtk_widget_destroy(dialog);

    if (created) {
        GtkWidget *info = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
            GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
            "Cuenta creada correctamente. Ingresa con tus nuevas credenciales.");
        gtk_dialog_run(GTK_DIALOG(info));
        gtk_widget_destroy(info);
        gtk_widget_grab_focus(g_login_password_entry);
    }
}

static void on_admin_manage_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    if (!g_current_user) {
        gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "login_view");
        gtk_widget_grab_focus(g_login_username_entry);
        return;
    }
    if (!g_user_role || g_strcmp0(g_user_role, "admin") != 0) {
        GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
            "Esta sección es solo para administradores.");
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        return;
    }
    refresh_admin_products();
    gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "admin_view");
}

static void refresh_admin_products(void) {
    if (!g_admin_list_box) return;
    clear_container(g_admin_list_box);

    if (!g_current_user || !g_user_role || g_strcmp0(g_user_role, "admin") != 0) {
        if (g_admin_status_label)
            gtk_label_set_text(GTK_LABEL(g_admin_status_label),
                "Inicia sesión como administrador para gestionar el inventario.");
        return;
    }

    char *resp = send_command("GET_ALL_PRODUCTS");
    if (!resp) {
        if (g_admin_status_label)
            gtk_label_set_text(GTK_LABEL(g_admin_status_label),
                "No se pudo obtener el inventario. Intenta más tarde.");
        return;
    }

    if (g_str_has_prefix(resp, "ERROR|")) {
        char *msg = resp + 6;
        char *newline = strchr(msg, '\n');
        if (newline) *newline = '\0';
        if (g_admin_status_label)
            gtk_label_set_text(GTK_LABEL(g_admin_status_label), msg);
        return;
    }

    if (g_str_has_prefix(resp, "EMPTY")) {
        GtkWidget *label = gtk_label_new("No hay productos actualmente.");
        gtk_list_box_insert(GTK_LIST_BOX(g_admin_list_box), label, -1);
        if (g_admin_status_label)
            gtk_label_set_text(GTK_LABEL(g_admin_status_label),
                "El inventario está vacío.");
        gtk_widget_show_all(g_admin_list_box);
        return;
    }

    char *copy = g_strdup(resp);
    char *saveptr;
    char *line = strtok_r(copy, "\n", &saveptr);
    int count = 0;
    while (line && *line) {
        char *sp;
        char *marca  = strtok_r(line, "|", &sp);
        char *modelo = strtok_r(NULL, "|", &sp);
        char *specs  = strtok_r(NULL, "|", &sp);
        char *precio = strtok_r(NULL, "|", &sp);
        if (marca && modelo && specs && precio) {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_widget_set_margin_top(row, 4);
            gtk_widget_set_margin_bottom(row, 4);

            GtkWidget *info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            char *title_markup = g_markup_printf_escaped("<span weight='bold'>%s %s</span>", marca, modelo);
            GtkWidget *title_label = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(title_label), title_markup);
            g_free(title_markup);

            GtkWidget *specs_label = gtk_label_new(specs);
            gtk_label_set_line_wrap(GTK_LABEL(specs_label), TRUE);

            GtkWidget *price_label = gtk_label_new(NULL);
            char price_text[64];
            snprintf(price_text, sizeof(price_text), "$ %s", precio);
            gtk_label_set_text(GTK_LABEL(price_label), price_text);

            gtk_box_pack_start(GTK_BOX(info_box), title_label, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(info_box), specs_label, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(info_box), price_label, FALSE, FALSE, 0);

            GtkWidget *remove_btn = gtk_button_new_with_label("Eliminar");
            gtk_widget_set_halign(remove_btn, GTK_ALIGN_END);
            gtk_widget_set_valign(remove_btn, GTK_ALIGN_CENTER);
            gtk_style_context_add_class(gtk_widget_get_style_context(remove_btn), "destructive-action");
            g_object_set_data_full(G_OBJECT(remove_btn), "model-name", g_strdup(modelo), g_free);
            g_signal_connect(remove_btn, "clicked", G_CALLBACK(on_admin_remove_clicked), NULL);

            gtk_box_pack_start(GTK_BOX(row), info_box, TRUE, TRUE, 0);
            gtk_box_pack_end(GTK_BOX(row), remove_btn, FALSE, FALSE, 0);

            gtk_list_box_insert(GTK_LIST_BOX(g_admin_list_box), row, -1);
            count++;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    g_free(copy);

    if (g_admin_status_label) {
        if (count > 0)
            gtk_label_set_text(GTK_LABEL(g_admin_status_label),
                "Da clic en \"Eliminar\" para remover un modelo.");
        else
            gtk_label_set_text(GTK_LABEL(g_admin_status_label),
                "No se encontraron productos activos.");
    }

    gtk_widget_show_all(g_admin_list_box);
}

static void on_admin_remove_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    const char *modelo = g_object_get_data(G_OBJECT(button), "model-name");
    if (!modelo) return;

    GtkWidget *confirm = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
        "¿Eliminar el modelo %s del inventario?", modelo);
    gtk_dialog_add_buttons(GTK_DIALOG(confirm), "Cancelar", GTK_RESPONSE_CANCEL,
        "Eliminar", GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(confirm), GTK_RESPONSE_ACCEPT);
    gint resp = gtk_dialog_run(GTK_DIALOG(confirm));
    gtk_widget_destroy(confirm);
    if (resp != GTK_RESPONSE_ACCEPT) return;

    char command[512];
    snprintf(command, sizeof(command), "REMOVE_PRODUCT:%s", modelo);
    char *server_resp = send_command(command);
    if (!server_resp) return;

    if (g_str_has_prefix(server_resp, "OK")) {
        if (g_admin_status_label)
            gtk_label_set_text(GTK_LABEL(g_admin_status_label), "Modelo eliminado correctamente.");
        refresh_admin_products();
    } else {
        const char *msg = "No se pudo eliminar el modelo.";
        if (g_str_has_prefix(server_resp, "ERROR|")) {
            msg = server_resp + 6;
            char *newline = strchr((char*)msg, '\n');
            if (newline) *newline = '\0';
        }
        GtkWidget *err = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "%s", msg);
        gtk_dialog_run(GTK_DIALOG(err));
        gtk_widget_destroy(err);
    }
}
