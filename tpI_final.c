// tpI_final.c
// Repositorio público (ejemplo, reemplazar por el tuyo):
// https://github.com/usuario/tpi_telegram_bot

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include <ctype.h>

// ----- Código base que NO conviene tocar -----
struct memory {
  char *response;
  size_t size;
};

static size_t cb(char *data, size_t size, size_t nmemb, void *clientp)
{
  size_t realsize = nmemb;
  struct memory *mem = clientp;

  char *ptr = realloc(mem->response, mem->size + realsize + 1);
  if(!ptr)
    return 0;  /* out of memory */

  mem->response = ptr;
  memcpy(&(mem->response[mem->size]), data, realsize);
  mem->size += realsize;
  mem->response[mem->size] = 0;

  return realsize;
}
// ----- Fin código base -----

#define LOG_FILE "bot_log.txt"

/* Lee el token desde un archivo (1ra línea) */
int leer_token(const char *filename, char *token, size_t maxlen)
{
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "No se pudo abrir el archivo de token: %s\n", filename);
        return 0;
    }

    if (!fgets(token, (int)maxlen, f)) {
        fprintf(stderr, "No se pudo leer el token del archivo.\n");
        fclose(f);
        return 0;
    }
    fclose(f);

    // Eliminar \n o \r\n
    size_t len = strlen(token);
    while (len > 0 && (token[len-1] == '\n' || token[len-1] == '\r')) {
        token[len-1] = '\0';
        len--;
    }

    if (len == 0) {
        fprintf(stderr, "El token leido está vacío.\n");
        return 0;
    }

    return 1;
}

/* Inicializa el buffer de memoria para libcurl */
void init_chunk(struct memory *chunk)
{
    chunk->response = NULL;
    chunk->size = 0;
}

/* Ejecuta una petición HTTP y devuelve el puntero a chunk->response */
int http_get(CURL *curl, struct memory *chunk, const char *url)
{
    CURLcode res;

    init_chunk(chunk);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)chunk);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "Error en curl_easy_perform: %d\n", res);
        if (chunk->response) {
            free(chunk->response);
            chunk->response = NULL;
        }
        return 0;
    }

    if (!chunk->response) {
        // No se recibió nada
        return 0;
    }

    return 1;
}

/* Parsea un campo string del JSON: "campo": "valor"  */
int parse_string_field(const char *json, const char *field_name, char *out, size_t maxlen)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", field_name);

    const char *p = strstr(json, pattern);
    if (!p) return 0;

    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;

    if (*p != '\"') return 0;
    p++; // saltar la comilla

    size_t i = 0;
    while (*p && *p != '\"' && i < maxlen - 1) {
        // manejo mínimo de escapes
        if (*p == '\\' && p[1]) {
            p++;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';

    return 1;
}

/* Parsea un campo numérico entero: "campo": 12345 */
int parse_long_field(const char *json, const char *field_name, long *value)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", field_name);

    const char *p = strstr(json, pattern);
    if (!p) return 0;

    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;

    *value = strtol(p, NULL, 10);
    return 1;
}

/* Loguea mensajes IN/OUT */
void log_message(const char *direction, long unix_time, const char *name, const char *text)
{
    FILE *f = fopen(LOG_FILE, "a");
    if (!f) return;

    if (!name) name = "-";
    if (!text) text = "-";

    // Formato: direccion;timestamp;nombre;mensaje
    fprintf(f, "%s;%ld;%s;%s\n", direction, unix_time, name, text);
    fclose(f);
}

/* Convierte una cadena a minúsculas (in-place) */
void to_lower_str(char *s)
{
    while (*s) {
        *s = (char)tolower((unsigned char)*s);
        s++;
    }
}

/* Codifica espacios como %20 (simple, suficiente para el TP) */
void url_encode_spaces(const char *msg, char *out, size_t maxlen)
{
    size_t j = 0;
    for (size_t i = 0; msg[i] != '\0' && j < maxlen - 1; i++) {
        if (msg[i] == ' ') {
            if (j + 3 >= maxlen) break; // no hay lugar
            out[j++] = '%';
            out[j++] = '2';
            out[j++] = '0';
        } else {
            out[j++] = msg[i];
        }
    }
    out[j] = '\0';
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <archivo_con_token>\n", argv[0]);
        return 1;
    }

    char token[256];
    if (!leer_token(argv[1], token, sizeof(token))) {
        return 1;
    }

    char base_url[512];
    snprintf(base_url, sizeof(base_url), "https://api.telegram.org/bot%s", token);

    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "No se pudo inicializar CURL.\n");
        return 1;
    }

    struct memory chunk;
    long offset_update_id = -1;  // -1: sin offset todavía

    printf("Bot iniciado. Esperando mensajes...\n");

    while (1) {
        char url_get[1024];

        if (offset_update_id >= 0) {
            snprintf(url_get, sizeof(url_get), "%s/getUpdates?offset=%ld", base_url, offset_update_id);
        } else {
            snprintf(url_get, sizeof(url_get), "%s/getUpdates", base_url);
        }

        if (!http_get(curl, &chunk, url_get)) {
            fprintf(stderr, "Fallo en getUpdates.\n");
            sleep(2);
            continue;
        }

        // Revisar si hay error general en JSON
        if (strstr(chunk.response, "\"ok\":false")) {
            fprintf(stderr, "La API devolvió un error: %s\n", chunk.response);
            free(chunk.response);
            chunk.response = NULL;
            sleep(2);
            continue;
        }

        // Si no hay mensajes nuevos: "result": []
        if (strstr(chunk.response, "\"result\": []")) {
            // Nada nuevo, esperamos 2 segundos y seguimos
            free(chunk.response);
            chunk.response = NULL;
            sleep(2);
            continue;
        }

        // Hay al menos un mensaje en result
        // Buscamos update_id
        long update_id = 0;
        if (!parse_long_field(chunk.response, "update_id", &update_id)) {
            // Si no se puede parsear, liberamos y seguimos
            free(chunk.response);
            chunk.response = NULL;
            sleep(2);
            continue;
        }

        // Buscamos la sección de "message"
        char *p_message = strstr(chunk.response, "\"message\"");
        if (!p_message) {
            free(chunk.response);
            chunk.response = NULL;
            // Marcamos como leído igual para no trabarnos
            offset_update_id = update_id + 1;
            sleep(2);
            continue;
        }

        // De "message" en adelante obtenemos chat, date, text, first_name
        long chat_id = 0;
        long msg_date = 0;
        char first_name[128] = "";
        char text[512] = "";

        // Primero, encontrar "chat" dentro de message
        char *p_chat = strstr(p_message, "\"chat\"");
        if (p_chat) {
            // id de chat
            parse_long_field(p_chat, "id", &chat_id);
            // first_name
            parse_string_field(p_chat, "first_name", first_name, sizeof(first_name));
        }

        // fecha unix del mensaje
        parse_long_field(p_message, "date", &msg_date);
        // texto del mensaje
        parse_string_field(p_message, "text", text, sizeof(text));

        // Logueamos mensaje recibido
        log_message("IN", msg_date, first_name, text);

        // Decidimos respuesta
        char respuesta[512] = "";
        if (text[0] != '\0') {
            char texto_lower[512];
            strncpy(texto_lower, text, sizeof(texto_lower) - 1);
            texto_lower[sizeof(texto_lower) - 1] = '\0';
            to_lower_str(texto_lower);

            if (strstr(texto_lower, "hola") != NULL) {
                snprintf(respuesta, sizeof(respuesta), "Hola, %s", first_name[0] ? first_name : "usuario");
            } else if (strstr(texto_lower, "chau") != NULL) {
                snprintf(respuesta, sizeof(respuesta), "Chau %s, que tengas buen día!", first_name[0] ? first_name : "");
            }
        }

        if (respuesta[0] != '\0' && chat_id != 0) {
            char respuesta_codificada[1024];
            url_encode_spaces(respuesta, respuesta_codificada, sizeof(respuesta_codificada));

            char url_send[2048];
            snprintf(
                url_send,
                sizeof(url_send),
                "%s/sendMessage?chat_id=%ld&text=%s",
                base_url,
                chat_id,
                respuesta_codificada
            );

            struct memory chunk_send;
            if (http_get(curl, &chunk_send, url_send)) {
                // Se podría chequear ok:true, pero para el TP no hace falta
                // Logueamos mensaje enviado (usamos la misma fecha o time(NULL))
                log_message("OUT", msg_date, "BOT", respuesta);
                free(chunk_send.response);
            } else {
                fprintf(stderr, "No se pudo enviar la respuesta.\n");
            }
        }

        // Actualizamos offset para no repetir mensaje
        offset_update_id = update_id + 1;

        // Liberamos respuesta del getUpdates
        if (chunk.response) {
            free(chunk.response);
            chunk.response = NULL;
        }

        sleep(2);  // consulta cada 2 segundos
    }

    // En la práctica nunca se llega acá, pero por prolijidad:
    curl_easy_cleanup(curl);
    return 0;
}
