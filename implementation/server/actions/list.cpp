#include "../../common/errors.h"
#include "../../common/seq.h"
#include "../../common/types.h"
#include "../../common/utils.h"
#include <filesystem>
#include <openssl/evp.h>
#include <string.h>
#include <sys/socket.h>
#include <tuple>

tuple<unsigned char *, unsigned int> get_file_list(char *username) {

    // get list of file from the user directory
    string list = "";
    string user(username);
    string path = std::filesystem::canonical(".") / "server/storage/" / user;
    for (const auto &entry : filesystem::directory_iterator(path)) {
        list += entry.path().filename();
        list += "\n";
    }

    // Convert string to uchar*
    unsigned char *file_list = new unsigned char[list.length()];
    for (int i = 0; i < list.length(); i++) {
        file_list[i] = (unsigned char)list[i];
    }

    return {file_list, list.length()};
}

void list_files(int sock, unsigned char *key, char *username) {

    // -----------receive client list request-----------

    auto server_header_res = read_header(sock);
    if (server_header_res.is_error) {
        handle_errors();
    }
    auto [seq, iv] = server_header_res.result;

    if (seq != seq_num) {
        delete[] iv;
        handle_errors("Incorrect sequence number");
    }

    // read ciphertext
    auto ct_res = read_field<uchar>(sock);
    if (ct_res.is_error) {
        delete[] iv;
        handle_errors("Incorrect message type");
    }
    auto [ct_len, ct] = ct_res.result;
    auto *pt = new unsigned char[sizeof(ct)];

    // read tag
    auto tag_res = read_field<uchar>(sock);
    if (tag_res.is_error) {
        delete[] ct;
        delete[] pt;
        delete[] iv;
        handle_errors("Incorrect message type");
    }
    auto [_, tag] = tag_res.result;

    EVP_CIPHER_CTX *ctx;
    if ((ctx = EVP_CIPHER_CTX_new()) == nullptr) {
        delete[] iv;
        delete[] ct;
        delete[] tag;
        delete[] pt;
        handle_errors("Could not decrypt message (alloc)");
    }
    int len;

    // Decrypt init
    if (EVP_DecryptInit(ctx, get_symmetric_cipher(), key, iv) != 1) {
        delete[] iv;
        delete[] ct;
        delete[] tag;
        delete[] pt;
        EVP_CIPHER_CTX_free(ctx);
        handle_errors();
    }

    delete[] iv;

    unsigned char header = mtype_to_uc(ListReq);

    /* Specify authenticated data */
    int err = 0;
    err |= EVP_DecryptUpdate(ctx, nullptr, &len, &header, sizeof(mtype));
    err |=
        EVP_DecryptUpdate(ctx, nullptr, &len, seqnum_to_uc(), sizeof(seqnum));

    if (err != 1) {
        delete[] ct;
        delete[] tag;
        delete[] pt;
        EVP_CIPHER_CTX_free(ctx);
        handle_errors();
    }

    // Encrypt Update: one call is enough because our mesage is very short.
    if (EVP_DecryptUpdate(ctx, pt, &len, ct, ct_len) != 1) {
        delete[] ct;
        delete[] tag;
        delete[] pt;
        EVP_CIPHER_CTX_free(ctx);
    }

    // GCM tag check
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_LEN, tag);

    // Encrypt Final. Finalize the encryption and adds the padding
    if (EVP_DecryptFinal(ctx, pt + len, &len) != 1) {
        delete[] ct;
        delete[] tag;
        delete[] pt;
        EVP_CIPHER_CTX_free(ctx);
    }

    // free variables
    delete[] ct;
    delete[] tag;
    delete[] pt;

    // free context
    EVP_CIPHER_CTX_free(ctx);

    inc_seqnum();

    // get user's file list
    auto [file_list, file_list_len] = get_file_list(username);

    // check file list length < max length of the packet data
    if (file_list_len + 1 > FLEN_MAX) {
        delete[] file_list;
        handle_errors("File list too long");
    }

#ifdef DEBUG
    cout << endl << GREEN << "File list from user's directory: " << endl;
    for (int i = 0; i < file_list_len; i++)
        printf("%c", file_list[i]);
    cout << RESET << endl;
#endif

    //-----------------Respond to client---------------------

    // Generate iv for message
    auto iv_res = gen_iv();
    if (iv_res.is_error) {
        delete[] file_list;
        handle_errors(iv_res.error);
    }
    iv = iv_res.result;

    auto send_packet_header_res =
        send_header(sock, ListAns, seq_num, iv, get_iv_len());
    if (send_packet_header_res.is_error) {
        delete[] file_list;
        delete[] iv;
        handle_errors(send_packet_header_res.error);
    }

    // Initialize encryption context
    len = 0;
    ct_len = 0;
    if ((ctx = EVP_CIPHER_CTX_new()) == nullptr) {
        delete[] file_list;
        delete[] iv;
        handle_errors("Could not encrypt message (alloc)");
    }

    if (EVP_EncryptInit(ctx, get_symmetric_cipher(), key, iv) != 1) {
        delete[] file_list;
        delete[] iv;
        EVP_CIPHER_CTX_free(ctx);
        handle_errors();
    }

    // Authenticate data
    err = 0;
    header = mtype_to_uc(ListAns);
    err |=
        EVP_EncryptUpdate(ctx, nullptr, &len, &header, sizeof(unsigned char));
    err |=
        EVP_EncryptUpdate(ctx, nullptr, &len, seqnum_to_uc(), sizeof(seqnum));
    if (err != 1) {
        delete[] file_list;
        delete[] iv;
        EVP_CIPHER_CTX_free(ctx);
        handle_errors();
    }

    // TODO: chuncks?
    // Encrypt file list
    ct = file_list;
    if (EVP_EncryptUpdate(ctx, ct, &len, file_list, file_list_len) !=
        1) { // TODO: len+1?
        delete[] file_list;
        delete[] iv;
        delete[] file_list;
        delete[] ct;
        EVP_CIPHER_CTX_free(ctx);
        handle_errors();
    }
    ct_len = len;

    if (EVP_EncryptFinal(ctx, ct + len, &len) != 1) {
        delete[] iv;
        delete[] file_list;
        delete[] ct;
        EVP_CIPHER_CTX_free(ctx);
        handle_errors();
    }
    ct_len += len;

    tag = new unsigned char[TAG_LEN];
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, TAG_LEN, tag) != 1) {
        delete[] iv;
        delete[] file_list;
        delete[] ct;
        delete[] tag;
        EVP_CIPHER_CTX_free(ctx);
        handle_errors();
    }
    delete[] iv;
    EVP_CIPHER_CTX_free(ctx);

    auto ct_send_res = send_field(sock, (flen)ct_len, ct);
    if (ct_send_res.is_error) {
        delete[] ct;
        delete[] tag;
        handle_errors(ct_send_res.error);
    }
    delete[] ct;

    auto tag_send_res = send_field(sock, (flen)TAG_LEN, tag);
    if (tag_send_res.is_error) {
        delete[] tag;
        handle_errors(tag_send_res.error);
    }
    delete[] tag;

    inc_seqnum();
}