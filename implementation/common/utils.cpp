#include "utils.h"
#include "types.h"
#include <errno.h>
#include <filesystem>
#include <iostream>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>
#include <tuple>
#include <unistd.h>

using namespace std;
namespace fs = std::filesystem;

void print_debug(unsigned char *x, int len) {
    for (int i = 0; i < len; i++)
        printf("%02x", (int)x[i]);
}

const EVP_CIPHER *get_symmetric_cipher() { return EVP_aes_256_gcm(); }
int get_iv_len() { return EVP_CIPHER_iv_length(get_symmetric_cipher()); }
int get_block_size() { return EVP_CIPHER_block_size(get_symmetric_cipher()); }

int get_symmetric_key_length() {
    auto cipher = get_symmetric_cipher();
    return EVP_CIPHER_key_length(cipher);
}

const EVP_MD *get_hash_type() { return EVP_sha256(); }
int get_hash_type_length() { return EVP_MD_size(get_hash_type()); }
int get_signature_max_length(EVP_PKEY *privkey) {
    return EVP_PKEY_size(privkey);
}

Maybe<unsigned char *> kdf(unsigned char *shared_secret, int shared_secret_len,
                           unsigned int key_len) {
    Maybe<unsigned char *> res;
    unsigned char *key = new unsigned char[key_len];

    unsigned char *digest = new unsigned char[get_hash_type_length()];
    unsigned int digest_len;
    EVP_MD_CTX *ctx;
    if ((ctx = EVP_MD_CTX_new()) == nullptr ||
        EVP_DigestInit(ctx, get_hash_type()) != 1 ||
        EVP_DigestUpdate(ctx, shared_secret, shared_secret_len) != 1 ||
        EVP_DigestFinal(ctx, digest, &digest_len) != 1) {
        delete[] key;
        delete[] digest;
        delete[] shared_secret;
        explicit_bzero(shared_secret, shared_secret_len);
        EVP_MD_CTX_free(ctx);
        res.set_error("Could not create hashing context for kdf");
        return res;
    }

    explicit_bzero(shared_secret, shared_secret_len);
    delete[] shared_secret;
    EVP_MD_CTX_free(ctx);

    if (digest_len < key_len) {
        delete[] key;
        delete[] digest;
        res.set_error("Cannot derive a key: key length is bigger than the "
                      "digest's length.");
        return res;
    }

    memcpy(key, digest, key_len);
    explicit_bzero(digest, digest_len);
    delete[] digest;

    res.set_result(key);
    return res;
}

Maybe<unsigned char *> gen_iv() {
    Maybe<unsigned char *> res;

    int iv_len = get_iv_len();
    unsigned char *iv = new unsigned char[iv_len];
    if (RAND_poll() != 1) {
        delete[] iv;
        res.set_error("Could not seed generator");
        return res;
    }

    if (RAND_bytes(iv, iv_len) != 1) {
        delete[] iv;
        res.set_error("Could not generate IV");
    } else {
        res.set_result(iv);
    }
    return res;
}

Maybe<unsigned char *> get_dummy() {
    Maybe<unsigned char *> res;
    unsigned char *dummy = new unsigned char[DUMMY_LEN];

    if (RAND_poll() != 1) {
        res.set_error("Could not seed generator");
        return res;
    }

    if (RAND_bytes(dummy, DUMMY_LEN) != 1) {
        res.set_error("Could not generate dummy");
    } else {
        res.set_result(dummy);
    }
    return res;
}

Maybe<mtypes> get_mtype(int socket) {
    Maybe<mtypes> res;

    if (read(socket, &res.result, sizeof(mtype)) != sizeof(mtype)) {
        res.set_error("Error when reading mtype");
    };

#ifdef DEBUG
    cout << endl
         << GREEN << "Message type: " << mtypes_to_string(res.result) << RESET
         << endl;
#endif
    return res;
}

Maybe<bool> send_header(int socket, mtypes type) {
    Maybe<bool> res;
    if (write(socket, &type, sizeof(mtype)) != sizeof(mtype)) {
        res.set_error("Error when writing mtype");
        return res;
    }
#ifdef DEBUG
    cout << endl
         << BLUE << "Message type: " << mtypes_to_string(mtypes(type)) << RESET
         << endl;
#endif
    res.set_result(true);
    return res;
}

Maybe<bool> send_header(int socket, mtypes type, seqnum seq_num, uchar *iv,
                        int iv_len) {
    auto res = send_header(socket, type);
    if (res.is_error) {
        return res;
    }

    if (write(socket, &seq_num, sizeof(seq_num)) != sizeof(seq_num)) {
        res.set_error("Error when writing sequence number");
        return res;
    }

#ifdef DEBUG
    cout << BLUE << "Sequence number: " << seq_num << RESET << endl;
#endif

    if (write(socket, iv, iv_len) != iv_len) {
        res.set_error("Error when writing iv");
        return res;
    };

#ifdef DEBUG
    cout << BLUE << "IV: ";
    print_debug(iv, iv_len);
    cout << RESET << endl;
#endif

    res.set_result(true);
    return res;
}

unsigned char mtype_to_uc(mtypes m) { return (unsigned char)m; }

Maybe<tuple<seqnum, unsigned char *>> read_header(int socket) {
    Maybe<tuple<seqnum, unsigned char *>> res;

    ssize_t received_len = 0;
    ssize_t read_len;
    seqnum seq;
    while ((unsigned long)received_len < sizeof(seqnum)) {
        if ((read_len = read(socket, (uchar *)&seq + received_len,
                             sizeof(seqnum) - received_len)) <= 0) {
            res.set_error("Error when reading sequence number");
            return res;
        }
        received_len += read_len;
    }

#ifdef DEBUG
    cout << GREEN << "Sequence number: " << seq << RESET << endl;
#endif

    received_len = 0;
    unsigned char *iv = new unsigned char[get_iv_len()];
    while (received_len < get_iv_len()) {
        if ((read_len = read(socket, iv + received_len,
                             get_iv_len() - received_len)) <= 0) {
            delete[] iv;
            res.set_error("Error when reading iv");
            return res;
        }

        received_len += read_len;
    }

#ifdef DEBUG
    cout << GREEN << "IV: ";
    print_debug(iv, get_iv_len());
    cout << RESET << endl;
#endif

    res.set_result({seq, iv});
    return res;
}

unsigned char *string_to_uchar(string s) {
    unsigned char *res = new unsigned char[s.length() + 1];
    memcpy(res, s.c_str(), s.length() + 1);
    return res;
}

fs::path get_user_storage_path(char *username) {
    return fs::current_path() / "server" / "storage" / username;
}

bool is_path_valid(char *username, fs::path user_path) {
    fs::path ok_path = get_user_storage_path(username);
    fs::path user_path_canonical = fs::weakly_canonical(user_path);

    return user_path_canonical.native().rfind(ok_path.native(), 0) == 0;
}

const char *mtypes_to_string(mtypes m) {
    switch (m) {
    case AuthStart:
        return "AuthStart";
    case AuthServerAns:
        return "AuthServerAns";
    case AuthClientAns:
        return "AuthClientAns";
    case UploadReq:
        return "UploadReq";
    case UploadAns:
        return "UploadAns";
    case UploadChunk:
        return "UploadChunk";
    case UploadEnd:
        return "UploadEnd";
    case UploadRes:
        return "UploadRes";
    case DownloadReq:
        return "DownloadReq";
    case DownloadChunk:
        return "DownloadChunk";
    case DownloadEnd:
        return "DownloadEnd";
    case DeleteReq:
        return "DeleteReq";
    case DeleteConfirm:
        return "DeleteConfirm";
    case DeleteAns:
        return "DeleteAns";
    case DeleteRes:
        return "DeleteRes";
    case ListReq:
        return "ListReq";
    case ListAns:
        return "ListAns";
    case RenameReq:
        return "RenameReq";
    case RenameAns:
        return "RenameAns";
    case LogoutReq:
        return "LogoutReq";
    case LogoutAns:
        return "LogoutAns";
    case Error:
        return "Error";
    }
}
