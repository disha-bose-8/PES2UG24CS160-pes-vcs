#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── IMPLEMENTATION ─────────────────────────────────────────────────────────

// WRITE OBJECT
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // 1. Header
    char header[64];
    const char *type_str =
        (type == OBJ_BLOB) ? "blob" :
        (type == OBJ_TREE) ? "tree" : "commit";

    int header_len = sprintf(header, "%s %zu", type_str, len) + 1;

    // 2. Full buffer (header + data)
    size_t total_len = header_len + len;
    unsigned char *buffer = malloc(total_len);
    if (!buffer) return -1;

    memcpy(buffer, header, header_len);
    memcpy(buffer + header_len, data, len);

    // 3. HASH (correct way)
    compute_hash(buffer, total_len, id_out);

    // 4. Dedup check
    if (object_exists(id_out)) {
        free(buffer);
        return 0;
    }

    // 5. Build path
    char path[512];
    object_path(id_out, path, sizeof(path));

    // 6. Create directories
    mkdir(PES_DIR, 0755);
    mkdir(OBJECTS_DIR, 0755);

    char dir[512];
    strncpy(dir, path, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0755);
    }

    // 7. Write file
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        free(buffer);
        return -1;
    }

    fwrite(buffer, 1, total_len, fp);
    fclose(fp);

    free(buffer);
    return 0;
}

// READ OBJECT
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    fseek(fp, 0, SEEK_END);
    size_t size = ftell(fp);
    rewind(fp);

    unsigned char *buffer = malloc(size);
    if (!buffer) {
        fclose(fp);
        return -1;
    }

    fread(buffer, 1, size, fp);
    fclose(fp);

    // find header/data separator
    unsigned char *null_pos = memchr(buffer, '\0', size);
    if (!null_pos) {
        free(buffer);
        return -1;
    }

    // parse type
    char type_str[10];
    sscanf((char *)buffer, "%s", type_str);

    if (strcmp(type_str, "blob") == 0) *type_out = OBJ_BLOB;
    else if (strcmp(type_str, "tree") == 0) *type_out = OBJ_TREE;
    else if (strcmp(type_str, "commit") == 0) *type_out = OBJ_COMMIT;
    else {
        free(buffer);
        return -1;
    }

    size_t data_len = size - (null_pos - buffer) - 1;

    *data_out = malloc(data_len);
    if (!*data_out) {
        free(buffer);
        return -1;
    }

    memcpy(*data_out, null_pos + 1, data_len);
    *len_out = data_len;

    free(buffer);
    return 0;
}
