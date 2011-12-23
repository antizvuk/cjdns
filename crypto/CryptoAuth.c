#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include "crypto_box_curve25519xsalsa20poly1305.h"
#include "crypto_core_hsalsa20.h"
#include "crypto_hash_sha256.h"
#include "crypto_scalarmult_curve25519.h"
#include "crypto_stream_salsa20.h"
#include "crypto_stream_xsalsa20.h"

#include "crypto/Crypto.h"
#include "crypto/CryptoAuth.h"
#include "crypto/CryptoAuth_struct.h"
#include "crypto/ReplayProtector.h"
#include "interface/Interface.h"
#include "libbenc/benc.h"
#include "memory/MemAllocator.h"
#include "util/Endian.h"
#include "wire/Headers.h"
#include "wire/Message.h"

#include <stdio.h>
#define DEBUG(x) printf(__FILE__ ":%u " x, __LINE__)
#define DEBUG1(x, y) printf(__FILE__ ":%u " x, __LINE__, y)

/** The constant used in nacl. */
static const uint8_t keyHashSigma[16] = "expand 32-byte k";
static const uint8_t keyHashNonce[16] = {0};

/**
 * Get a shared secret.
 *
 * @param outputSecret an array to place the shared secret in.
 * @param myPrivateKey
 * @param herPublicKey
 * @param passwordHash a 32 byte value known to both ends, this must be provably pseudorandom
 *                     the first 32 bytes of a sha256 output from hashing a password is ok,
 *                     whatever she happens to send me in the Auth field is NOT ok.
 *                     If this field is null, the secret will be generated without the password.
 */
static inline void getSharedSecret(uint8_t outputSecret[32],
                                   uint8_t myPrivateKey[32],
                                   uint8_t herPublicKey[32],
                                   uint8_t passwordHash[32])
{
    uint8_t tempBuff[64];
    crypto_scalarmult_curve25519(tempBuff, myPrivateKey, herPublicKey);
    if (passwordHash == NULL) {
        crypto_core_hsalsa20(outputSecret, keyHashNonce, tempBuff, keyHashSigma);
    } else {
        memcpy(&tempBuff[32], passwordHash, 32);
        crypto_hash_sha256(outputSecret, tempBuff, 64);
    }
}

static inline void hashPassword_sha256(struct Auth* auth, const String* password)
{
    uint8_t tempBuff[32];
    crypto_hash_sha256(auth->secret, (uint8_t*) password->bytes, password->len);
    crypto_hash_sha256(tempBuff, auth->secret, 32);
    memcpy(auth->challenge.bytes, tempBuff, Headers_AuthChallenge_SIZE);
    Headers_setAuthChallengeDerivations(&auth->challenge, 0);
    auth->challenge.challenge.type = 1;
}

static inline uint8_t* hashPassword(struct Auth* auth,
                                    const String* password,
                                    const uint8_t authType)
{
    switch (authType) {
        case 1:
            hashPassword_sha256(auth, password);
    };
    return auth->secret;
}

/**
 * Search the authorized passwords for one matching this auth header.
 *
 * @param auth the auth header.
 * @param context the CryptoAuth engine to search in.
 * @return an Auth struct with a if one is found, otherwise NULL.
 */
static inline struct Auth* getAuth(union Headers_AuthChallenge auth, struct CryptoAuth* context)
{
    if (auth.challenge.type != 1) {
        return NULL;
    }
    for (uint32_t i = 0; i < context->passwordCount; i++) {
        if (memcmp(auth.bytes, &context->passwords[i], Headers_AuthChallenge_KEYSIZE) == 0) {
            return &context->passwords[i];
        }
    }
    return NULL;
}

static inline void getPasswordHash(uint8_t output[32], uint8_t derivations, struct Auth* auth)
{
    uint8_t tempBuff[32];
    memcpy(output, auth->secret, 32);
    for (uint32_t i = 0; i < derivations; i++) {
        crypto_hash_sha256(tempBuff, output, 32);
        crypto_hash_sha256(output, tempBuff, 32);
    }
}

static inline uint8_t* tryAuth(union Headers_CryptoAuth* cauth,
                               uint8_t hashOutput[32],
                               struct Wrapper* wrapper,
                               void** userPtr)
{
    struct Auth* auth = getAuth(cauth->handshake.auth, wrapper->context);
    if (auth != NULL) {
        uint16_t deriv = Headers_getAuthChallengeDerivations(&cauth->handshake.auth);
        getPasswordHash(hashOutput, deriv, auth);
        if (deriv == 0) {
            *userPtr = auth->user;
        }
        return hashOutput;
    }
    wrapper->authenticatePackets |= Headers_isPacketAuthRequired(&cauth->handshake.auth);

    return NULL;
}

/**
 * Decrypt and authenticate.
 *
 * @param nonce a 24 byte number, may be random, cannot repeat.
 * @param msg a message to encipher and authenticate.
 * @param secret a shared secret.
 */
static inline int decryptRndNonce(uint8_t nonce[24],
                                  struct Message* msg,
                                  uint8_t secret[32])
{
    if (msg->length < 16) {
        return -1;
    }
    assert(msg->padding >= 16);
    uint8_t* startAt = msg->bytes - 16;
    uint8_t paddingSpace[16];
    memcpy(paddingSpace, startAt, 16);
    memset(startAt, 0, 16);
    if (crypto_box_curve25519xsalsa20poly1305_open_afternm(
            startAt, startAt, msg->length + 16, nonce, secret) != 0)
    {
        return -1;
    }

    memcpy(startAt, paddingSpace, 16);
    Message_shift(msg, -16);
    return 0;
}

/**
 * Encrypt and authenticate.
 * Shifts the message by 16 bytes.
 *
 * @param nonce a 24 byte number, may be random, cannot repeat.
 * @param msg a message to encipher and authenticate.
 * @param secret a shared secret.
 */
static inline void encryptRndNonce(uint8_t nonce[24],
                                   struct Message* msg,
                                   uint8_t secret[32])
{
    assert(msg->padding >= 32);

    // This function trashes 16 bytes of the padding so we will put it back
    uint8_t paddingSpace[16];
    memcpy(paddingSpace, msg->bytes - 32, 16);
    memset(msg->bytes - 32, 0, 32);
    crypto_box_curve25519xsalsa20poly1305_afternm(
        msg->bytes - 32, msg->bytes - 32, msg->length + 32, nonce, secret);

    memcpy(msg->bytes - 32, paddingSpace, 16);
    Message_shift(msg, 16);
}

/**
 * Encipher the content without any authentication.
 * Encryption is the same function as decryption.
 *
 * @param nonce a number which is used only once.
 * @param msg a message to encipher.
 * @param secret a shared secret.
 */
static inline int cipher(uint8_t nonce[8],
                          struct Message* msg,
                          uint8_t secret[20])
{
    return crypto_stream_salsa20_xor(msg->bytes, msg->bytes, msg->length, nonce, secret);
}

/**
 * Decrypt a packet.
 *
 * @param nonce a counter.
 * @param msg the message to decrypt, decrypted in place.
 * @param secret the shared secret.
 * @param isInitiator true if we started the connection.
 * @param authenticate if true then the packet will be authenticated as well at a 16 byte cost.
 *                     The packet must have been encrypted using authenticate.
 */
static inline int decrypt(uint32_t nonce,
                          struct Message* msg,
                          uint8_t secret[32],
                          bool isInitiator,
                          bool authenticate)
{
    union {
        uint32_t ints[2];
        uint8_t bytes[24];
    } nonceAs = { .ints = {0, 0} };
    nonceAs.ints[!isInitiator] = nonce;

    if (authenticate) {
        return decryptRndNonce(nonceAs.bytes, msg, secret);
    } else {
        return cipher(nonceAs.bytes, msg, secret);
    }
}

/**
 * Encrypt a packet.
 *
 * @param nonce a counter.
 * @param msg the message to decrypt, decrypted in place.
 * @param secret the shared secret.
 * @param isInitiator true if we started the connection.
 * @param authenticate if true then the packet will be authenticated as well at a 16 byte cost.
 *                     The packet must have been encrypted using authenticate.
 */
static inline void encrypt(uint32_t nonce,
                           struct Message* msg,
                           uint8_t secret[32],
                           bool isInitiator,
                           bool authenticate)
{
    union {
        uint32_t ints[2];
        uint8_t bytes[24];
    } nonceAs = { .ints = {0, 0} };
    nonceAs.ints[isInitiator] = nonce;

    if (authenticate) {
        encryptRndNonce(nonceAs.bytes, msg, secret);
    } else {
        cipher(nonceAs.bytes, msg, secret);
    }
}

static inline void setRequiredPadding(struct Wrapper* wrapper)
{
    uint32_t padding = (wrapper->nextNonce < 4) ? 36 : sizeof(union Headers_CryptoAuth) + 32;
    wrapper->externalInterface.requiredPadding =
        wrapper->wrappedInterface->requiredPadding + padding;
    wrapper->externalInterface.maxMessageLength =
        wrapper->wrappedInterface->maxMessageLength - padding;
}

static inline bool knowHerKey(struct Wrapper* wrapper)
{
    uint64_t* herKey = (uint64_t*) wrapper->herPerminentPubKey;
    return (herKey[0] | herKey[1] | herKey[2] | herKey[3]) != 0;
}

/**
 * If we don't know her key, the handshake has to be done backwards.
 * Reverse handshake requests are signaled by sending a non-obfuscated zero nonce.
 */
static uint8_t genReverseHandshake(struct Message* message,
                                   struct Wrapper* wrapper,
                                   union Headers_CryptoAuth* header)
{
    wrapper->nextNonce = 0;
    Message_shift(message, -Headers_CryptoAuth_SIZE);

    // Buffer the packet so it can be sent ASAP
    if (wrapper->bufferedMessage == NULL) {
        DEBUG("Buffered a message.\n");
        wrapper->bufferedMessage =
            Message_clone(message, wrapper->externalInterface.allocator);
        assert(wrapper->nextNonce == 0);
    } else {
        DEBUG("Expelled a message because a session has not yet been setup.\n");
        Message_copyOver(wrapper->bufferedMessage,
                         message,
                         wrapper->externalInterface.allocator);
        assert(wrapper->nextNonce == 0);
    }

    Message_shift(message, Headers_CryptoAuth_SIZE);
    header = (union Headers_CryptoAuth*) message->bytes;
    header->nonce = UINT32_MAX;
    memcpy(&header->handshake.publicKey, wrapper->context->publicKey, 32);

    message->length = Headers_CryptoAuth_SIZE;

    // sessionState must be 0, auth and 24 byte nonce are garbaged and public key is set
    // now garbage the authenticator and the encrypted key which are not used.
    randombytes((uint8_t*) &header->handshake.authenticator, 48);

    return wrapper->wrappedInterface->sendMessage(message, wrapper->wrappedInterface);
}

static uint8_t encryptHandshake(struct Message* message, struct Wrapper* wrapper)
{
    assert(message->padding >= sizeof(union Headers_CryptoAuth) || !"not enough padding");

    Message_shift(message, sizeof(union Headers_CryptoAuth));

    union Headers_CryptoAuth* header = (union Headers_CryptoAuth*) message->bytes;

    // garbage the auth field to frustrate DPI and set the nonce (next 24 bytes after the auth)
    randombytes((uint8_t*) &header->handshake.auth, sizeof(union Headers_AuthChallenge) + 24);

    if (!knowHerKey(wrapper)) {
        return genReverseHandshake(message, wrapper, header);
    }

    // Password auth
    uint8_t* passwordHash = NULL;
    if (wrapper->password != NULL) {
        struct Auth auth;
        passwordHash = hashPassword(&auth, wrapper->password, wrapper->authType);
        memcpy(header->handshake.auth.bytes, &auth.challenge, sizeof(union Headers_AuthChallenge));
    }
    header->handshake.auth.challenge.type = wrapper->authType;

    Headers_setPacketAuthRequired(&header->handshake.auth, wrapper->authenticatePackets);

    // set the session state
    header->nonce = Endian_hostToBigEndian32(wrapper->nextNonce);

    if (wrapper->nextNonce == 0 || wrapper->nextNonce == 2) {
        // If we're sending a hello or a key
        crypto_box_curve25519xsalsa20poly1305_keypair(header->handshake.encryptedTempKey,
                                                      wrapper->secret);
    } else if (wrapper->nextNonce == 3) {
        // Dupe key
        // If nextNonce is 1 then we have our pubkey stored in wrapper->tempKey,
        // If nextNonce is 3 we need to recalculate it each time
        // because tempKey the final secret.
        crypto_scalarmult_curve25519_base(header->handshake.encryptedTempKey,
                                          wrapper->secret);
    } else {
        // Dupe hello
        // wrapper->nextNonce == 1
        // Our public key is cached in wrapper->tempKey so lets copy it out.
        memcpy(header->handshake.encryptedTempKey, wrapper->tempKey, 32);
    }

    uint8_t sharedSecret[32];
    if (wrapper->nextNonce < 2) {
        if (wrapper->nextNonce == 0) {
            DEBUG("Sending hello packet\n");
        } else {
            DEBUG("Sending repeat hello packet\n");
        }
        getSharedSecret(sharedSecret,
                        wrapper->context->privateKey,
                        wrapper->herPerminentPubKey,
                        passwordHash);
        wrapper->isInitiator = true;
        wrapper->nextNonce = 1;
        // just generated this, copy it into tempKey so it is available for handshake2.
        memcpy(wrapper->tempKey, header->handshake.encryptedTempKey, 32);
    } else {
        if (wrapper->nextNonce == 2) {
            DEBUG("Sending key packet\n");
        } else {
            DEBUG("Sending repeat key packet\n");
        }
        // Handshake2 wrapper->tempKey holds her public temp key.
        // it was put there by receiveMessage()
        getSharedSecret(sharedSecret,
                        wrapper->context->privateKey,
                        wrapper->tempKey,
                        passwordHash);
        wrapper->nextNonce = 3;
    }

    // Shift message over the encryptedTempKey field.
    Message_shift(message, 32 - ((int32_t) sizeof(union Headers_CryptoAuth)));

    // This function uses space in the "padding" section of the message
    // which has the effect of zeroing the publicKey field
    encryptRndNonce(header->handshake.nonce, message, sharedSecret);

    memcpy(header->handshake.publicKey, wrapper->context->publicKey, 32);

    // Shift it back -- encryptRndNonce adds 16 bytes of padding.
    Message_shift(message, sizeof(union Headers_CryptoAuth) - 32 - 16);

    return wrapper->wrappedInterface->sendMessage(message, wrapper->wrappedInterface);

}

static inline uint8_t encryptMessage(struct Message* message,
                                     struct Wrapper* wrapper)
{
    assert(message->padding >= 36 || !"not enough padding");

    encrypt(wrapper->nextNonce,
            message,
            wrapper->secret,
            wrapper->isInitiator,
            wrapper->authenticatePackets);

    Message_shift(message, 4);

    union Headers_CryptoAuth* header = (union Headers_CryptoAuth*) message->bytes;
    header->nonce = Endian_hostToBigEndian32(wrapper->nextNonce);
    wrapper->nextNonce++;

    return wrapper->wrappedInterface->sendMessage(message, wrapper->wrappedInterface);
}

static uint8_t sendMessage(struct Message* message, struct Interface* interface)
{
    struct Wrapper* wrapper = (struct Wrapper*) interface->senderContext;

    // nextNonce 0: sending hello, we are initiating connection.
    // nextNonce 1: sending another hello, nothing received yet.
    // nextNonce 2: sending key, hello received.
    // nextNonce 3: sending key again, no data packet recieved yet.
    // nextNonce >3: handshake complete
    //
    // if it's a blind handshake, every message will be empty and nextNonce will remain
    // zero until the first message is received back.

    if (wrapper->nextNonce < 5) {
        if (wrapper->nextNonce < 4) {
            return encryptHandshake(message, wrapper);
        } else {
            getSharedSecret(wrapper->secret,
                            wrapper->secret,
                            wrapper->tempKey, NULL);
        }
    }
    return encryptMessage(message, wrapper);
}

/** Call the external interface and tell it that a message has been received. */
static inline void callReceivedMessage(struct Wrapper* wrapper, struct Message* message)
{
    if (wrapper->externalInterface.receiveMessage != NULL) {
        wrapper->externalInterface.receiveMessage(message, &wrapper->externalInterface);
    }
}

static inline bool decryptMessage(struct Wrapper* wrapper,
                                  uint32_t nonce,
                                  struct Message* content,
                                  uint8_t secret[32])
{
    if (wrapper->authenticatePackets) {
        // Decrypt with authentication and replay prevention.
        if (decrypt(nonce, content, secret, wrapper->isInitiator, true) == 0
            && ReplayProtector_checkNonce(nonce, &wrapper->replayProtector))
        {
            if (nonce == 4) {
                wrapper->nextNonce += 3;
            }
            callReceivedMessage(wrapper, content);
            return true;
        }
    } else {
        // Decrypt and send whatever garbage comes out the other end!
        decrypt(nonce, content, secret, wrapper->isInitiator, false);
        if (nonce == 4 && wrapper->nextNonce < 4) {
            // While this is safe against packet drops, that safety is lost if a packet
            // is forged with nonce=4 during a handshake.
            wrapper->nextNonce += 3;
        }
        callReceivedMessage(wrapper, content);
        return true;
    }
    return false;
}

static void decryptHandshake(struct Wrapper* wrapper,
                             uint32_t nonce,
                             struct Message* message,
                             union Headers_CryptoAuth* header)
{
    if (message->length < sizeof(union Headers_CryptoAuth)) {
        DEBUG("Dropped runt packet\n");
        return;
    }

    // handshake
    // nextNonce 0: recieving hello.
    // nextNonce 1: recieving key, we sent hello.
    // nextNonce 2: recieving first data packet or duplicate hello.
    // nextNonce 3: recieving first data packet.
    // nextNonce >3: handshake complete

    if (wrapper->nextNonce < 2 && nonce == UINT32_MAX && !wrapper->requireAuth) {
        // Reset without knowing key is allowed until state reaches 2.
        // this is because we don't know that the other end knows our key until we
        // have received a valid packet from them.
        // We can't allow the upper layer to see this message because it's not authenticated.
        if (!knowHerKey(wrapper)) {
            memcpy(wrapper->herPerminentPubKey, header->handshake.publicKey, 32);
        }
        Message_shift(message, -Headers_CryptoAuth_SIZE);
        message->length = 0;
        wrapper->nextNonce = 0;
        wrapper->user = NULL;
        // Send an empty response (to initiate the connection).
        encryptHandshake(message, wrapper);
        return;
    }

    void* user = NULL;
    uint8_t passwordHashStore[32];
    uint8_t* passwordHash = tryAuth(header, passwordHashStore, wrapper, &user);
    if (wrapper->requireAuth && !user) {
        DEBUG("Dropping message because auth was not given and is required.\n");
        return;
    }

    // What the nextNonce will become if this packet is valid.
    uint32_t nextNonce;

    // The secret for decrypting this message.
    uint8_t sharedSecret[32];

    uint8_t* herPermKey = NULL;
    if (nonce < 2) {
        if (nonce == 0) {
            DEBUG("Received a hello packet\n");
        } else {
            DEBUG("Received a repeat hello packet\n");
        }

        // TODO: Prevent this from happening on connections which are active to prevent RST attacks.

        // Decrypt message with perminent keys.
        if (!knowHerKey(wrapper)) {
            herPermKey = header->handshake.publicKey;
        } else {
            herPermKey = wrapper->herPerminentPubKey;
        }

        getSharedSecret(sharedSecret,
                        wrapper->context->privateKey,
                        herPermKey,
                        passwordHash);
        nextNonce = 2;
    } else {
        if (nonce == 2) {
            DEBUG("Received a key packet\n");
        } else if (nonce == 3) {
            DEBUG("Received a repeat key packet\n");
        } else {
            DEBUG1("Received a packet of unknown type! nonce=%u\n", nonce);
        }
        // We sent the hello, this is a key
        getSharedSecret(sharedSecret,
                        wrapper->secret,
                        wrapper->herPerminentPubKey,
                        passwordHash);
        nextNonce = 4;
    }

    // Shift it on top of the authenticator before the encrypted public key
    Message_shift(message, 48 - Headers_CryptoAuth_SIZE);

    // Decrypt her temp public key and the message.
    if (decryptRndNonce(header->handshake.nonce, message, sharedSecret) != 0) {
        // just in case
        memset(header, 0, Headers_CryptoAuth_SIZE);
        DEBUG("Dropped message because authenticated decryption failed.\n");
        return;
    }

    wrapper->user = user;
    memcpy(wrapper->tempKey, header->handshake.encryptedTempKey, 32);
    Message_shift(message, -32);
    wrapper->nextNonce = nextNonce;
    if (herPermKey && herPermKey != wrapper->herPerminentPubKey) {
        memcpy(wrapper->herPerminentPubKey, herPermKey, 32);
    }

    // If this is a handshake which was initiated in reverse because we
    // didn't know the other node's key, now send what we were going to send.
    if (wrapper->bufferedMessage && message->length == 0) {
        DEBUG("Sending buffered message.\n");
        sendMessage(wrapper->bufferedMessage, &wrapper->externalInterface);
        return;
    } else if (wrapper->bufferedMessage) {
        DEBUG("There is a buffered message");
    }

    setRequiredPadding(wrapper);
    callReceivedMessage(wrapper, message);
    return;
}

static void receiveMessage(struct Message* received, struct Interface* interface)
{
    struct Wrapper* wrapper = (struct Wrapper*) interface->receiverContext;
    union Headers_CryptoAuth* header = (union Headers_CryptoAuth*) received->bytes;

    if (received->length < (wrapper->requireAuth ? 20 : 4)) {
        DEBUG("Dropped runt");
        return;
    }
    assert(received->padding >= 12 || "need at least 12 bytes of padding in incoming message");


    Message_shift(received, -4);

    uint32_t nonce = Endian_bigEndianToHost32(header->nonce);

    if (wrapper->nextNonce < 5) {
        DEBUG("Got handshake.\n");
        if (nonce > 3 && nonce != UINT32_MAX) {
            DEBUG1("Trying final handshake step, nonce=%u\n", nonce);
            uint8_t secret[32];
            getSharedSecret(secret,
                            wrapper->secret,
                            wrapper->tempKey,
                            NULL);
            if (decryptMessage(wrapper, nonce, received, secret)) {
                wrapper->nextNonce += 2;
                memcpy(wrapper->secret, secret, 32);
                return;
            }
            DEBUG("Final handshake step failed.\n");
        }
    } else if (decryptMessage(wrapper, nonce, received, wrapper->secret)) {
        // If decryptMessage returns false then we will try the packet as a handshake.
        return;
    } else {
        DEBUG("Decryption failed, trying message as a handshake.\n");
    }
    Message_shift(received, 4);
    decryptHandshake(wrapper, nonce, received, header);
}

/////////////////////////////////////////////////////////////////////////////////////////////////

struct CryptoAuth* CryptoAuth_new(struct MemAllocator* allocator,
                                  const uint8_t* privateKey)
{
    struct CryptoAuth* ca = allocator->calloc(sizeof(struct CryptoAuth), 1, allocator);
    ca->allocator = allocator;

    ca->passwords = allocator->calloc(sizeof(struct Auth), 256, allocator);
    ca->passwordCount = 0;
    ca->passwordCapacity = 256;

    if (privateKey != NULL) {
        memcpy(ca->privateKey, privateKey, 32);
        crypto_scalarmult_curve25519_base(ca->publicKey, ca->privateKey);
    } else {
        crypto_box_curve25519xsalsa20poly1305_keypair(ca->publicKey, ca->privateKey);
    }

    return ca;
}

int32_t CryptoAuth_addUser(String* password,
                           uint8_t authType,
                           void* user,
                           struct CryptoAuth* context)
{
    if (authType != 1) {
        return -1;
    }
    if (context->passwordCount == context->passwordCapacity) {
        // TODO: realloc password space and increase buffer.
        return -2;
    }
    hashPassword_sha256(&context->passwords[context->passwordCount], password);
    context->passwords[context->passwordCount].user = user;
    context->passwordCount++;
    return 0;
}

void* CryptoAuth_getUser(struct Interface* interface)
{
    return ((struct Wrapper*) interface->senderContext)->user;
}

struct Interface* CryptoAuth_wrapInterface(struct Interface* toWrap,
                                           uint8_t herPublicKey[32],
                                           const bool requireAuth,
                                           bool authenticatePackets,
                                           struct CryptoAuth* context)
{
    struct Wrapper* wrapper =
        toWrap->allocator->clone(sizeof(struct Wrapper), toWrap->allocator, &(struct Wrapper) {
            .user = NULL,
            .nextNonce = 0,
            .context = context,
            .wrappedInterface = toWrap,
            .requireAuth = requireAuth,
            .authenticatePackets = authenticatePackets
        });

    toWrap->receiverContext = wrapper;
    toWrap->receiveMessage = receiveMessage;

    struct Interface iface = {
        .senderContext = wrapper,
        .sendMessage = sendMessage,
        .allocator = toWrap->allocator
    };
    memcpy(&wrapper->externalInterface, &iface, sizeof(struct Interface));

    if (herPublicKey != NULL) {
        memcpy(wrapper->herPerminentPubKey, herPublicKey, 32);
    }

    return &wrapper->externalInterface;
}

void CryptoAuth_setAuth(const String* password,
                        const uint8_t authType,
                        struct Interface* wrappedInterface)
{
    struct Wrapper* wrapper = (struct Wrapper*) wrappedInterface->senderContext;
    wrapper->password = (password != NULL)
        ? benc_newBinaryString(password->bytes, password->len, wrappedInterface->allocator)
        : NULL;
    wrapper->authType = (password != NULL) ? authType : 0;
}

void CryptoAuth_getPublicKey(uint8_t output[32], struct CryptoAuth* context)
{
    memcpy(output, context->publicKey, 32);
}

uint8_t* CryptoAuth_getHerPublicKey(struct Interface* interface)
{
    return ((struct Wrapper*) interface->senderContext)->herPerminentPubKey;
}

void CryptoAuth_getSession(struct Session* output, struct Interface* interface)
{
    struct Wrapper* wrapper = (struct Wrapper*) interface->senderContext;
    output->isInitiator = wrapper->isInitiator;
    output->nextNonce = wrapper->nextNonce;
    memcpy(output->sharedSecret, wrapper->secret, 32);
assert(!wrapper->authenticatePackets);
    output->exists = true;
}