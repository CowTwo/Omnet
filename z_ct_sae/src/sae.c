/*
 * sae.c
 *
 *  Created on: Dec 29, 2017
 *      Author: root
 */


#include <stdio.h>
#include <stdlib.h>

#include <libconfig.h>
#include <string.h>
#include "sae.h"

#define COUNTER_INFINITY        65535

#define state_to_string(x) (x) == SAE_NOTHING ? "NOTHING" : \
                           (x) == SAE_COMMITTED ? "COMMITTED" : \
                           (x) == SAE_CONFIRMED ? "CONFIRMED" : \
                           (x) == SAE_ACCEPTED ? "ACCEPTED" : \
                           "unknown"
#define seq_to_string(x) (x) == SAE_AUTH_COMMIT ? "COMMIT" : \
                         (x) == SAE_AUTH_CONFIRM ? "CONFIRM" : \
                         "unknown"

#define status_to_string(x) (x) == WLAN_STATUS_ANTI_CLOGGING_TOKEN_NEEDED ? "TOKEN NEEDED" : \
                            (x) == WLAN_STATUS_NOT_SUPPORTED_GROUP ? "REJECTION" : \
                            "unknown"


unsigned int function_mdlen = SHA256_DIGEST_LENGTH;

/*
 * the functions H() and CN()
 */
#define H_Init(ctx,x,l) HMAC_Init((ctx), (x), (l), EVP_sha256())
#define H_Update(ctx,x,l) HMAC_Update((ctx),(x),(l))
#define H_Final(ctx,x) HMAC_Final((ctx), (x), &function_mdlen)

#define CN_Init(ctx,x,l) HMAC_Init((ctx), (x), (l), EVP_sha256())
#define CN_Update(ctx,x,l) HMAC_Update((ctx),(x),(l))
#define CN_Final(ctx,x) HMAC_Final((ctx), (x), &function_mdlen)

/*
 * global variables
 */
//BN_CTX *bnctx = NULL;
//GD *gd;                                 /* group definitions */
//BIO *out;
//char allzero[SHA256_DIGEST_LENGTH];
//unsigned long token_generator;
//int curr_open, open_threshold, retrans;
//unsigned long blacklist_timeout, giveup_threshold, pmk_expiry;
//unsigned int sae_debug_mask;
//static int next_candidate_id = 0;

//TAILQ_HEAD(fubar, candidate) blacklist;
//TAILQ_HEAD(blah, candidate) peers;

int sae_env_init(P_SAE_CTX_T pSaeCtx)
{
    char *conffile = (char*)"/work/C_Project/openssl_trial/authsae.sample.cfg";
    char *meshid=(char*)"";
    struct sae_config sae_conf;

    if (conffile) {
        struct config_t cfg;
        struct config_setting_t *section;

        config_init(&cfg);
        if (!config_read_file(&cfg, conffile)) {
            fprintf(stderr, "Failed to load config file %s: %s\n", conffile,
                    config_error_text(&cfg));
            return -1;
        }
        section = config_lookup(&cfg, "authsae.sae");
        if (section == NULL) {
            fprintf(stderr, "Config file has not sae section\n");
            return -1;
        }

        if (sae_parse_libconfig(section, &sae_conf) != 0) {
            fprintf(stderr, "Failed to parse SAE configuration.\n");
            return -1;
        }

        config_destroy(&cfg);
    }

    printf("ct_MyMac:" MACSTR "\n", MAC2STR(pSaeCtx->myMac));
    printf("sae_conf.pwd=%s\n", sae_conf.pwd);
    printf("sae_conf.num_groups=%d\n", sae_conf.num_groups);
    DumpHex(sae_conf.group, sae_conf.num_groups*sizeof(int));

    if (sae_initialize(pSaeCtx, meshid, &sae_conf) < 0) {
        fprintf(stderr, "cannot configure SAE, check config file!\n");
        return -1;
    }
    return 1;
}

int saeInitiateCommit2Peer(P_SAE_CTX_T pSaeCtx)
{
    struct candidate *peer;

    if ((peer = create_candidate(pSaeCtx, &pSaeCtx->herMac[0], &pSaeCtx->myMac[0], 0, (void*)0/*cookie*/)) == NULL) {
        return -1;
    }
    peer->cookie = (void*)NULL/*cookie*/;
    /*
     * assign the first group in the list as the one to try
     */
    if (assign_group_to_peer(pSaeCtx, peer, pSaeCtx->gd) < 0) {
        //fin(WLAN_STATUS_UNSPECIFIED_FAILURE, peer->peer_mac, NULL, 0, peer->cookie);
        //delete_peer(&peer);
        return -1;
    } else {
        commit_to_peer(pSaeCtx, peer, NULL, 0);
        //peer->t0 = srv_add_timeout(srvctx, SRV_SEC(retrans), retransmit_peer, peer);
        peer->state = SAE_COMMITTED;
        fprintf(stderr, "state of " MACSTR " is now (%d) %s\n\n",
                  MAC2STR(peer->peer_mac), peer->state, state_to_string(peer->state));
    }
    return 1;
}

static enum result
process_authentication_frame (P_SAE_CTX_T pSaeCtx, struct candidate *peer,
        struct ieee80211_mgmt_frame *frame, int len)
{
    unsigned short grp;
    unsigned short seq = ieee_order(frame->authenticate.auth_seq);
    unsigned short status = ieee_order(frame->authenticate.status);
    GD *group_def;
    //struct candidate *delme;
    enum result ret;

    fprintf(stderr, "recv'd %s from " MACSTR " while in %s\n",
              seq_to_string(seq), MAC2STR(frame->sa),
              state_to_string(peer->state));

    //srv_rem_timeout(srvctx, peer->t0);
    //peer->t0 = 0;
    /*
     * implement the state machine for SAE
     */
    switch (peer->state) {
        case SAE_NOTHING:
            switch (seq) {
                case SAE_AUTH_COMMIT:
                    /*
                     * if the status is anything other than 0 then throw this away
                     * since as far as we're concerned this is unsolicited and there's
                     * no error we committed.
                     */
                    if (status != 0) {
                        return ERR_FATAL;
                    }
                    /*
                     * grab the group from the frame...
                     */
                    grp = ieee_order(*((frame->authenticate.u.var16)));
                    /*
                     * ...and see if it's supported
                     */
                    group_def = pSaeCtx->gd;
                    while (group_def) {
                        if (grp == group_def->group_num) {
                            if (assign_group_to_peer(pSaeCtx, peer, group_def) < 0) {
                                return ERR_FATAL;
                            }
                            break;
                        }
                        group_def = group_def->next;
                    }
                    if (group_def == NULL) {
                        /*
                         * send a rejection to the peer and a "del" event to the parent
                         */
                        fprintf(stderr,  "group %d not supported, reject.\n", grp);
                        //reject_to_peer(peer, frame);
                        return ERR_FATAL;
                    }
                    fprintf(stderr,  "COMMIT received for unknown peer " MACSTR ", committing and confirming\n",
                    MAC2STR(peer->peer_mac));
                    peer->sc = peer->rc = 0;
                    commit_to_peer(pSaeCtx, peer, NULL, 0);
                    if (process_commit(pSaeCtx, peer, frame, len) < 0) {
                        return ERR_FATAL;
                    }
                    /*
                     * send both a commit and a confirm and transition into confirmed
                     */
                    confirm_to_peer(pSaeCtx, peer);
                    peer->sync = 0;
                    //peer->t0 = srv_add_timeout(srvctx, SRV_SEC(retrans), retransmit_peer, peer);
                    peer->state = SAE_CONFIRMED;
                    break;
                case SAE_AUTH_CONFIRM:
                    return ERR_FATAL;
                default:
                    fprintf(stderr,  "unknown SAE frame (%d) from " MACSTR "\n",
                           seq, MAC2STR(peer->peer_mac));
                    return ERR_NOT_FATAL;
            }
            break;
        case SAE_COMMITTED:
            switch (seq) {
                case SAE_AUTH_COMMIT:
                    /*
                     * grab the group from the frame, we need it later
                     */
                    grp = ieee_order(*((frame->authenticate.u.var16)));
                    /*
                     * if the group offered is not the same as what we offered
                     * that means the commit messages crossed in the ether. Check
                     * whether this is something we can support and if so tie break.
                     */
                    if (grp != peer->grp_def->group_num) {
                        return ERR_FATAL;
                    } else {
                        if (process_commit(pSaeCtx, peer, frame, len) < 0) {
                            return ERR_FATAL;
                        }
                    }
                    confirm_to_peer(pSaeCtx, peer);
                    //peer->t0 = srv_add_timeout(srvctx, SRV_SEC(retrans), retransmit_peer, peer);
                    peer->state = SAE_CONFIRMED;
                    break;
#if 0
                case SAE_AUTH_CONFIRM:
                    sae_debug(SAE_DEBUG_STATE_MACHINE, "got CONFIRM before COMMIT, try again\n");
                    if (peer->sync > giveup_threshold) {
                        return ERR_FATAL;
                    }
                    peer->sync++;
                    commit_to_peer(peer, NULL, 0);
                    peer->t0 = srv_add_timeout(srvctx, SRV_SEC(retrans), retransmit_peer, peer);
                    break;
#endif
                default:
                    fprintf(stderr,  "unknown SAE frame (%d) from " MACSTR "\n",
                              seq, MAC2STR(peer->peer_mac));
                    return ERR_NOT_FATAL;
            }
            break;
        case SAE_CONFIRMED:
            switch (seq) {
                case SAE_AUTH_COMMIT:
                    return ERR_FATAL;

                    break;
                case SAE_AUTH_CONFIRM:
                    ret = (enum result)process_confirm(pSaeCtx, peer, frame, len);
                    switch (ret) {
                        case ERR_FATAL:
                        case ERR_BLACKLIST:
                            return ret;
                        case ERR_NOT_FATAL:                   /* this is not in 11s draft */
                            return NO_ERR;
                        case NO_ERR:
                            fprintf(stderr, "process_confirm success\n");
                            break;
                    }
#if 0
                    curr_open--;
                    peer->sc = COUNTER_INFINITY;
                    if ((delme = find_peer(peer->peer_mac, 1)) != NULL) {
                        fprintf(stderr,
                                  "peer " MACSTR " in %s has just ACCEPTED, found another in %s, deleting\n",
                                  MAC2STR(peer->peer_mac),
                                  state_to_string(peer->state), state_to_string(delme->state));
                        delete_peer(&delme);
                    }
#endif
                    /*
                     * print out the PMK if we have debugging on for that
                     */
                    if (peer->state != SAE_ACCEPTED) {
                        if (1/*sae_debug_mask & SAE_DEBUG_CRYPTO*/) {
                            print_buffer((char*)"PMK", peer->pmk, SHA256_DIGEST_LENGTH);
                        }
                        //fin(WLAN_STATUS_SUCCESSFUL, peer->peer_mac, peer->pmk, SHA256_DIGEST_LENGTH, peer->cookie);
                    }
                    //fprintf(stderr, "setting reauth timer for %d seconds\n", pmk_expiry);
                    peer->state = SAE_ACCEPTED;
                    break;
                default:
                    fprintf(stderr, "unknown SAE frame (%d) from " MACSTR "\n",
                              seq, MAC2STR(peer->peer_mac));
                    return ERR_NOT_FATAL;
            }
            break;
        case SAE_ACCEPTED:
#if 0
            switch (seq) {
                case SAE_AUTH_COMMIT:
                    /*
                     * something stinks in state machine land...
                     */
                    break;
                case SAE_AUTH_CONFIRM:
                    if (peer->sync > giveup_threshold) {
                        sae_debug(SAE_DEBUG_STATE_MACHINE, "too many syncronization errors on " MACSTR ", deleting\n",
                                  MAC2STR(peer->peer_mac));
                        return ERR_FATAL;
                    }
                    /*
                     * must've lost our confirm, check if it's old or invalid,
                     * if neither send confirm again....
                     */
                    if (check_confirm(peer, frame) &&
                        (process_confirm(peer, frame, len) >= 0)) {
                        sae_debug(SAE_DEBUG_STATE_MACHINE, "peer " MACSTR " resending CONFIRM...\n",
                        MAC2STR(peer->peer_mac));
                        peer->sync++;
                        confirm_to_peer(peer);
                    }
                    break;
                default:
                    sae_debug(SAE_DEBUG_ERR, "unknown SAE frame (%d) from " MACSTR "\n",
                              seq, MAC2STR(peer->peer_mac));
                    return ERR_NOT_FATAL;
            }
#endif
            break;
    }
    fprintf(stderr,  "state of " MACSTR " is now (%d) %s\n\n",
              MAC2STR(peer->peer_mac), peer->state, state_to_string(peer->state));
    return NO_ERR;
}

int
process_mgmt_frame (P_SAE_CTX_T pSaeCtx, struct ieee80211_mgmt_frame *frame, int len,
        unsigned char *me, void *cookie)
{
    unsigned short frame_control, type/*, auth_alg*/;
    //int need_token;
    struct candidate *peer;
    enum result ret = ERR_FATAL;

    if (pSaeCtx->bnctx == NULL) {
        /*
         * sae_initialize() has not been called yet!
         */
        fprintf(stderr, "sae_initialize() must be called first!\n");
        return -1;
    }
    frame_control = ieee_order(frame->frame_control);
    type = IEEE802_11_FC_GET_TYPE(frame_control);
    if (type != IEEE802_11_FC_TYPE_MGMT) {
        /*
         * we should only get management frames
         */
        return -1;
    }

    /*
     * process the management frame
     */
#if 1
    peer = find_peer(pSaeCtx, frame->sa, 0);
    switch (ieee_order(frame->authenticate.auth_seq)) {
        case SAE_AUTH_COMMIT:
            if ((peer != NULL) && (peer->state != SAE_ACCEPTED)) {
                ret = process_authentication_frame(pSaeCtx, peer, frame, len);
            } else {
                /*
                 * if we got here that means we're not demanding tokens or we are
                 * and the token was correct. In either case we create a protocol instance.
                 */
                if ((peer = create_candidate(pSaeCtx, frame->sa, me, 0/*(curr_open >= open_threshold)*/, cookie)) == NULL) {
                    fprintf(stderr,  "can't malloc space for candidate from " MACSTR "\n",
                              MAC2STR(frame->sa));
                    return -1;
                }
                ret = process_authentication_frame(pSaeCtx, peer, frame, len);
            }
            break;
        case SAE_AUTH_CONFIRM:
            if (peer == NULL) {
                /*
                 * no peer instance, no way to handle this frame!
                 */
                return 0;
            }
            /*
             * since we searched above with "0" the peer we've handled the case
             * of two peers in the db with one in ACCEPTED state already.
             */
            ret = process_authentication_frame(pSaeCtx, peer, frame, len);
            break;
    }
#else
    switch (IEEE802_11_FC_GET_STYPE(frame_control)) {
        /*
         * a beacon sort of like an "Initiate" event
         */
        case IEEE802_11_FC_STYPE_BEACON:
            /*
             * ignore blacklisted peers
             */
            if (on_blacklist(frame->sa)) {
                break;
            }
            if ((peer = find_peer(frame->sa, 0)) != NULL) {
                /*
                 * we're already dealing with this guy, ignore his beacons now
                 */
                peer->beacons++;
            } else {
                /*
                 * This is actually not part of the parent state machine but handling
                 * it here makes the rest of the protocol instance state machine nicer.
                 *
                 * a new mesh point! auth_req transitions from state "NOTHING" to "COMMITTED"
                 */
                sae_debug(SAE_DEBUG_PROTOCOL_MSG, "received a beacon from " MACSTR "\n", MAC2STR(frame->sa));
                sae_debug(SAE_DEBUG_STATE_MACHINE, "Initiate event\n");
                if ((peer = create_candidate(frame->sa, me, 0, cookie)) == NULL) {
                    return -1;
                }
                peer->cookie = cookie;
                /*
                 * assign the first group in the list as the one to try
                 */
                if (assign_group_to_peer(peer, gd) < 0) {
                    fin(WLAN_STATUS_UNSPECIFIED_FAILURE, peer->peer_mac, NULL, 0, peer->cookie);
                    delete_peer(&peer);
                } else {
                    commit_to_peer(peer, NULL, 0);
                    peer->t0 = srv_add_timeout(srvctx, SRV_SEC(retrans), retransmit_peer, peer);
                    peer->state = SAE_COMMITTED;
                    sae_debug(SAE_DEBUG_STATE_MACHINE, "state of " MACSTR " is now (%d) %s\n\n",
                              MAC2STR(peer->peer_mac), peer->state, state_to_string(peer->state));
                }
                break;
            }
            break;
        case IEEE802_11_FC_STYPE_AUTH:
            auth_alg = ieee_order(frame->authenticate.alg);
            if (auth_alg != SAE_AUTH_ALG) {
                sae_debug(SAE_DEBUG_PROTOCOL_MSG,
                          "let kernel handle authenticate (%d) frame from " MACSTR " to " MACSTR "\n",
                          auth_alg, MAC2STR(frame->sa), MAC2STR(frame->da));
                break;
            }
            peer = find_peer(frame->sa, 0);
            switch (ieee_order(frame->authenticate.auth_seq)) {
                case SAE_AUTH_COMMIT:
                    if ((peer != NULL) && (peer->state != SAE_ACCEPTED)) {
                        ret = process_authentication_frame(peer, frame, len);
                    } else {
                        /*
                         * check if this is the same scalar that was sent when we
                         * accepted.
                         */
                        if ((peer != NULL) && (peer->state == SAE_ACCEPTED)) {
                            if (check_dup(peer, 0, frame, len) == 0) {
                                return 0;
                            }
                        }
                        /*
                         * if we are currently in a token-demanding state then check for a token
                         */
                        if (!(curr_open < open_threshold)) {
                            need_token = have_token(frame, len, me);
                            if (need_token < 0) {
                                /*
                                 * silently drop nonsense frames
                                 */
                                return 0;
                            } else if (need_token > 0) {
                                /*
                                 * request a token if the frame should have one but didn't
                                 */
                                sae_debug(SAE_DEBUG_STATE_MACHINE, "token needed for COMMIT (%d open), requesting one\n",
                                          curr_open);
                                request_token(frame, me, cookie);
                                return 0;
                            } else {
                                sae_debug(SAE_DEBUG_STATE_MACHINE, "correct token received\n");
                            }
                        }
                        /*
                         * if we got here that means we're not demanding tokens or we are
                         * and the token was correct. In either case we create a protocol instance.
                         */
                        if ((peer = create_candidate(frame->sa, me, (curr_open >= open_threshold), cookie)) == NULL) {
                            sae_debug(SAE_DEBUG_ERR, "can't malloc space for candidate from " MACSTR "\n",
                                      MAC2STR(frame->sa));
                            return -1;
                        }
                        ret = process_authentication_frame(peer, frame, len);
                    }
                    break;
                case SAE_AUTH_CONFIRM:
                    if (peer == NULL) {
                        /*
                         * no peer instance, no way to handle this frame!
                         */
                        return 0;
                    }
                    /*
                     * since we searched above with "0" the peer we've handled the case
                     * of two peers in the db with one in ACCEPTED state already.
                     */
                    ret = process_authentication_frame(peer, frame, len);
                    break;
            }
            switch (ret) {
                case ERR_BLACKLIST:
                    /*
                     * a "del" event
                     */
                    blacklist_peer(peer);
                    fin(WLAN_STATUS_UNSPECIFIED_FAILURE, peer->peer_mac, NULL, 0, peer->cookie);
                    /* no break / fall-through intentional */
                case ERR_FATAL:
                    /*
                     * a "fail" event, it could be argued that fin() should be done
                     * here but there are a certain class of failures-- group rejection
                     * for instance-- that don't really need fin() notification because
                     * the protocol might recover and successfully finish later.
                     */
                    delete_peer(&peer);
                    return 0;
                case ERR_NOT_FATAL:
                    /*
                     * This isn't in the 11s draft but when there is some internal error from
                     * an API call it's not really a protocol error. These things can (should?)
                     * be handled with a "fail" event but let's try and be a little more accomodating.
                     *
                     * if we get a non-fatal error return to NOTHING but don't delete yet, this way we
                     * won't try to authenticate her again when we see a beacon but will respond to an
                     * initiation from her later.
                     */
                    peer->failed_auth++;
                    peer->sync = peer->sc = peer->rc = 0;
                    peer->state = SAE_NOTHING;
                    break;
                case NO_ERR:
                    break;
            }
            if (peer->sync > giveup_threshold) {
                /*
                 * if the state machines are so out-of-whack just declare failure
                 */
                sae_debug(SAE_DEBUG_STATE_MACHINE,
                          "too many state machine syncronization errors, adding " MACSTR " to blacklist\n",
                          MAC2STR(peer->peer_mac));
                blacklist_peer(peer);
                fin(WLAN_STATUS_REQUEST_DECLINED, peer->peer_mac, NULL, 0, peer->cookie);
                delete_peer(&peer);
            }
            break;
        case IEEE802_11_FC_STYPE_ACTION:
            return process_ampe_frame(frame, len, me, cookie);
        default:
            return -1;
    }
#endif

    return 0;
}

void DumpHex(const void* data, size_t size) {
    char ascii[17];
    size_t i, j;
    ascii[16] = '\0';
    for (i = 0; i < size; ++i) {
        printf("%02X ", ((unsigned char*)data)[i]);
        if (((unsigned char*)data)[i] >= ' ' && ((unsigned char*)data)[i] <= '~') {
            ascii[i % 16] = ((unsigned char*)data)[i];
        } else {
            ascii[i % 16] = '.';
        }
        if ((i+1) % 8 == 0 || i+1 == size) {
            printf(" ");
            if ((i+1) % 16 == 0) {
                printf("|  %s \n", ascii);
            } else if (i+1 == size) {
                ascii[(i+1) % 16] = '\0';
                if ((i+1) % 16 <= 8) {
                    printf(" ");
                }
                for (j = (i+1) % 16; j < 16; ++j) {
                    printf("   ");
                }
                printf("|  %s \n", ascii);
            }
        }
    }
}


static void
dump_buffer (unsigned char *buf, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        if (i && (i%4 == 0)) {
            printf(" ");
        }
        if (i && (i%32 == 0)) {
            printf("\n");
        }
        printf("%02x", buf[i]);
    }
    printf("\n");
}

void
print_buffer (char *str, unsigned char *buf, int len)
{
    printf("%s:\n", str);
    dump_buffer(buf, len);
    printf("\n");
}

static void
pp_a_bignum (char *str, BIGNUM *bn)
{
    unsigned char *buf;
    int len;

    len = BN_num_bytes(bn);
    if ((buf = (unsigned char*)malloc(len)) == NULL) {
        return;
    }
    BN_bn2bin(bn, buf);
    print_buffer(str, buf, len);
    free(buf);
}

struct candidate *
find_peer (P_SAE_CTX_T pSaeCtx, unsigned char *mac, int accept)
{
    struct candidate *peer, *found = NULL;

    TAILQ_FOREACH(peer, &pSaeCtx->peers, entry) {
        if (memcmp(peer->peer_mac, mac, ETH_ALEN) == 0) {
            /*
             * if "accept" then we're only looking for peers in "accepted" state
             */
            if (accept) {
                if (peer->state == SAE_ACCEPTED) {
                    return peer;
                }
                continue;
            }
            /*
             * otherwise we'll take any peer but, if there are 2, give preference
             * to the one not in "accepted" state
             */
            if (found == NULL) {
                found = peer;
            } else {
                if ((found->state == SAE_ACCEPTED) &&
                    (peer->state != SAE_ACCEPTED)) {
                    found = peer;
                }
            }
        }
    }
    return found;
}

static int
compute_group_definition (P_SAE_CTX_T pSaeCtx, GD *grp, char *password, unsigned short num)
{
    BIGNUM *cofactor = NULL;
    int nid, ret = 0;

    switch (num) {        /* from IANA registry for IKE D-H groups */
        case 19:
            nid = NID_X9_62_prime256v1;
            break;
        case 20:
            nid = NID_secp384r1;
            break;
        case 21:
            nid = NID_secp521r1;
            break;
        case 25:
            nid = NID_X9_62_prime192v1;
            break;
        case 26:
            nid = NID_secp224r1;
            break;
        default:
            printf("unsupported group %d\n", num);
            return -1;
    }
    if ((grp->group = EC_GROUP_new_by_curve_name(nid)) == NULL) {
        puts("unable to create EC_GROUP!\n");
        return -1;
    }
    cofactor = NULL; grp->order = NULL; grp->prime = NULL;

    if (((cofactor = BN_new()) == NULL) ||
        ((grp->order = BN_new()) == NULL) ||
        ((grp->prime = BN_new()) == NULL)) {
        puts("unable to create bignums!\n");
        goto fail;
    }
    if (!EC_GROUP_get_curve_GFp(grp->group, grp->prime, NULL, NULL, pSaeCtx->bnctx)) {
        puts("unable to get prime for GFp curve!\n");
        goto fail;
    }
    if (!EC_GROUP_get_order(grp->group, grp->order, pSaeCtx->bnctx)) {
        puts("unable to get order for curve!\n");
        goto fail;
    }
    if (!EC_GROUP_get_cofactor(grp->group, cofactor, pSaeCtx->bnctx)) {
        puts("unable to get cofactor for curve!\n");
        goto fail;
    }
    if (BN_cmp(cofactor, BN_value_one())) {
        /*
         * no curves with a co-factor > 1 are allowed. Technically they will work
         * but the special handling of them to deal with a small sub-group attack
         * when compared to how many of them are in this IANA registry (as of writing
         * this that number is zero) makes it better to just not support them.
         */
        puts("attempting to use a curve with a co-factor > 1!\n");
        goto fail;
    }
    grp->group_num = num;
    strncpy(grp->password, password, sizeof(grp->password));

    if (0) {
fail:
        BN_free(grp->order);
        BN_free(grp->prime);
        ret = -1;
    }
    BN_free(cofactor);

    return ret;
}

int
sae_initialize (P_SAE_CTX_T pSaeCtx, char *ourssid, struct sae_config *config)
{
    GD *curr, *prev = NULL;
    int i;

    /* TODO: detect duplicate calls.  validate config. */

    EVP_add_digest(EVP_sha256());
    if ((pSaeCtx->out = BIO_new(BIO_s_file())) == NULL) {
        fprintf(stderr, "SAE: unable to create file BIO!\n");
        return -1;
    }
    BIO_set_fp(pSaeCtx->out, stderr, BIO_NOCLOSE);

    /*
     * initialize globals
     */
    memset(pSaeCtx->allzero, 0, SHA256_DIGEST_LENGTH);
#if 0
JC: Commented out until we decide whether this is needed (in which case we must
    be prepared to accept a binary, non-null terminated mesh ID) or not (the
    mesh_ssid is not used anywhere in this module, so maybe it can be dumped).

    memcpy(mesh_ssid, ourssid, strlen(ourssid));
#endif
    TAILQ_INIT(&pSaeCtx->peers);
    TAILQ_INIT(&pSaeCtx->blacklist);
    RAND_pseudo_bytes((unsigned char *)&pSaeCtx->token_generator, sizeof(unsigned long));
    if ((pSaeCtx->bnctx = BN_CTX_new()) == NULL) {
        fprintf(stderr, "cannot create bignum context!\n");
        return -1;
    }
    /*
     * set defaults and read in config
     */
    pSaeCtx->sae_debug_mask = config->debug;
    pSaeCtx->curr_open = 0;
    pSaeCtx->open_threshold = config->open_threshold ? config->open_threshold : 5;
    pSaeCtx->blacklist_timeout = config->blacklist_timeout ? config->blacklist_timeout
        : 30;
    pSaeCtx->giveup_threshold = config->giveup_threshold ? config->giveup_threshold :
        5;
    pSaeCtx->retrans = config->retrans ? config->retrans : 3;
    pSaeCtx->pmk_expiry = config->pmk_expiry ? config->pmk_expiry : 86400; /* one day */
    /*
     * create groups from configuration data
     */
    for (i=0; i<config->num_groups; i++) {
        if ((curr = (GD *)malloc(sizeof(GD))) == NULL) {
            puts("cannot malloc group definition!\n");
            return -1;
        }
        if (compute_group_definition(pSaeCtx, curr, config->pwd, config->group[i])) {
            free(curr);
            continue;
        }
        if (prev)
            prev->next = curr;
        else
            pSaeCtx->gd = curr;
        prev = curr;
        curr->next = NULL;
#if 1
        printf("group %d is configured, prime is %d"
                " bytes\n", curr->group_num, BN_num_bytes(curr->prime));
#endif
    }
    return 1;
}

int
sae_parse_libconfig (struct config_setting_t *sae_section, struct sae_config* config)
{
    struct config_setting_t *setting, *group;
    char *pwd;

    memset(config, 0, sizeof(struct sae_config));
    config_setting_lookup_int(sae_section, "debug", (config_int_t *)&config->debug);
    setting = config_setting_get_member(sae_section, "group");
    if (setting != NULL) {
        while (1) {
            group = config_setting_get_elem(setting, config->num_groups);
            if (!group)
                break;
            config->group[config->num_groups] =
                config_setting_get_int_elem(setting, config->num_groups);
            config->num_groups++;
            if (config->num_groups == SAE_MAX_EC_GROUPS)
                break;
        }
    }
    if (config_setting_lookup_string(sae_section, "password", (const char **)&pwd)) {
        strncpy(config->pwd, pwd, SAE_MAX_PASSWORD_LEN);
        if (config->pwd[SAE_MAX_PASSWORD_LEN - 1] != 0) {
            fprintf(stderr, "WARNING: Truncating password\n");
            config->pwd[SAE_MAX_PASSWORD_LEN - 1] = 0;
        }
    }
    config_setting_lookup_int(sae_section, "retrans", (config_int_t *)&config->retrans);
    config_setting_lookup_int(sae_section, "lifetime", (config_int_t *)&config->pmk_expiry);
    config_setting_lookup_int(sae_section, "thresh", (config_int_t *)&config->open_threshold);
    config_setting_lookup_int(sae_section, "blacklist", (config_int_t *)&config->blacklist_timeout);
    config_setting_lookup_int(sae_section, "giveup", (config_int_t *)&config->giveup_threshold);
    return 0;
}

/*
 * calculate the legendre symbol (a/p)
 */
static int
legendre (BIGNUM *a, BIGNUM *p, BIGNUM *exp, BN_CTX *bnctx)
{
    BIGNUM *tmp = NULL;
    int symbol = -1;

    if ((tmp = BN_new()) != NULL) {
        BN_mod_exp(tmp, a, exp, p, bnctx);
        if (BN_is_word(tmp, 1))
                symbol = 1;
        else if (BN_is_zero(tmp))
                symbol = 0;
        else
                symbol = -1;

        BN_free(tmp);
    }
    return symbol;
}
int
prf (unsigned char *key, int keylen, unsigned char *label, int labellen,
     unsigned char *context, int contextlen,
     unsigned char *result, int resultbitlen)
{
    HMAC_CTX ctx;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    int resultlen, len = 0;
    unsigned int mdlen = SHA256_DIGEST_LENGTH;
    unsigned char mask = 0xff;
    unsigned short reslength;
    unsigned short i = 0, i_le;

    reslength = ieee_order(resultbitlen);
    resultlen = (resultbitlen + 7)/8;
    do {
        i++;
        HMAC_Init(&ctx, key, keylen, EVP_sha256());
        i_le = ieee_order(i);
        HMAC_Update(&ctx, (unsigned char *) &i_le, sizeof(i_le));
        HMAC_Update(&ctx, label, labellen);
        HMAC_Update(&ctx, context, contextlen);
        HMAC_Update(&ctx, (unsigned char *)&reslength, sizeof(unsigned short));
        HMAC_Final(&ctx, digest, &mdlen);
        if ((len + mdlen) > resultlen) {
            memcpy(result+len, digest, resultlen - len);
        } else {
            memcpy(result+len, digest, mdlen);
        }
        len += mdlen;
        HMAC_CTX_cleanup(&ctx);
    } while (len < resultlen);
    /*
     * we're expanding to a bit length, if this is not a
     * multiple of 8 bits then mask off the excess.
     */
    if (resultbitlen % 8) {
        mask <<= (8 - (resultbitlen % 8));
        result[resultlen - 1] &= mask;
    }
    return resultlen;
}

struct candidate *
create_candidate(P_SAE_CTX_T pSaeCtx, unsigned char *her_mac, unsigned char *my_mac, unsigned short got_token, void *cookie)
{
    struct candidate *peer;

    if ((peer = (struct candidate *)malloc(sizeof(struct candidate))) == NULL) {
        fprintf(stderr,"can't malloc space for candidate!\n");
        return NULL;
    }
    memset(peer, 0, sizeof(*peer));
    memcpy(peer->my_mac, my_mac, ETH_ALEN);
    memcpy(peer->peer_mac, her_mac, ETH_ALEN);
    if (((peer->peer_scalar = BN_new()) == NULL) ||
        ((peer->my_scalar = BN_new()) == NULL)) {
        fprintf(stderr, "can't create peer data structures!\n");
        free(peer);
        return NULL;
    }
    peer->got_token = got_token;
    peer->failed_auth = peer->beacons = peer->state = peer->sync = peer->sc = peer->rc = 0;
    peer->private_val = NULL;
    peer->pwe = peer->peer_element = peer->my_element = NULL;
    TAILQ_INSERT_TAIL(&pSaeCtx->peers, peer, entry);
    peer->state = SAE_NOTHING;
    peer->cookie = cookie;
    peer->candidate_id = pSaeCtx->next_candidate_id++;
    pSaeCtx->curr_open++;

    //peer_created(her_mac);

    return peer;
}


/*
 * assign_group_tp_peer()
 *      The group has been selected, assign it to the peer and create PWE.
 */
int
assign_group_to_peer (P_SAE_CTX_T pSaeCtx, struct candidate *peer, GD *grp)
{
    HMAC_CTX ctx;
    BIGNUM *x_candidate = NULL, *x = NULL, *rnd = NULL, *qr = NULL, *qnr = NULL;
    BIGNUM *pm1 = NULL, *pm1d2 = NULL, *tmp1 = NULL, *tmp2 = NULL, *a = NULL, *b = NULL;
    unsigned char pwe_digest[SHA256_DIGEST_LENGTH], addrs[ETH_ALEN * 2], ctr;
    unsigned char *prfbuf = NULL, *primebuf = NULL;
    int primebitlen, is_odd, check, found = 0;

    /*
     * allow for replacement of group....
     */
    EC_POINT_free(peer->pwe);
    peer->pwe = NULL;
    EC_POINT_free(peer->peer_element);
    peer->peer_element = NULL;
    EC_POINT_free(peer->my_element);
    peer->my_element = NULL;
    BN_free(peer->private_val);
    peer->private_val = NULL;

    if (((rnd = BN_new()) == NULL) ||
        ((pm1d2 = BN_new()) == NULL) ||
        ((pm1 = BN_new()) == NULL) ||
        ((tmp1 = BN_new()) == NULL) ||
        ((tmp2 = BN_new()) == NULL) ||
        ((a = BN_new()) == NULL) ||
        ((b = BN_new()) == NULL) ||
        ((qr = BN_new()) == NULL) ||
        ((qnr = BN_new()) == NULL) ||
        ((x_candidate = BN_new()) == NULL)) {
        fprintf(stderr,  "can't create bignum for candidate!\n");
        goto fail;
    }
    peer->grp_def = grp;
    peer->pwe = EC_POINT_new(grp->group);
    peer->peer_element = EC_POINT_new(grp->group);
    peer->my_element = EC_POINT_new(grp->group);

    if ((prfbuf = (unsigned char *)malloc(BN_num_bytes(grp->prime))) == NULL) {
        fprintf(stderr,  "unable to malloc space for prf buffer!\n");
        BN_free(rnd);
        BN_free(x_candidate);
    goto fail;
    }
    if ((primebuf = (unsigned char *)malloc(BN_num_bytes(grp->prime))) == NULL) {
        fprintf(stderr, "unable to malloc space for prime!\n");
        free(prfbuf);
        BN_free(rnd);
        BN_free(x_candidate);
    goto fail;
    }
    BN_bn2bin(grp->prime, primebuf);
    primebitlen = BN_num_bits(grp->prime);

    if (!EC_GROUP_get_curve_GFp(grp->group, NULL, a, b, NULL)) {
        free(prfbuf);
    }

    BN_sub(pm1, grp->prime, BN_value_one());
    BN_copy(tmp1, BN_value_one());
    BN_add(tmp1, tmp1, BN_value_one());
    BN_div(pm1d2, tmp2, pm1, tmp1, pSaeCtx->bnctx);      /* (p-1)/2 */

    /*
     * generate a random quadratic residue modulo p and a random
     * quadratic non-residue modulo p.
     */
    do {
        BN_rand_range(qr, pm1);
    } while (legendre(qr, grp->prime, pm1d2, pSaeCtx->bnctx) != 1);
    do {
        BN_rand_range(qnr, pm1);
    } while (legendre(qnr, grp->prime, pm1d2, pSaeCtx->bnctx) != -1);
    memset(prfbuf, 0, BN_num_bytes(grp->prime));

    fprintf(stderr,  "computing PWE on %d bit curve number %d\n", primebitlen, grp->group_num);
    ctr = 0;
    while (ctr < 40) {
        ctr++;
        /*
         * compute counter-mode password value and stretch to prime
         */
        if (memcmp(peer->peer_mac, peer->my_mac, ETH_ALEN) > 0) {
            memcpy(addrs, peer->peer_mac, ETH_ALEN);
            memcpy(addrs+ETH_ALEN, peer->my_mac, ETH_ALEN);
        } else {
            memcpy(addrs, peer->my_mac, ETH_ALEN);
            memcpy(addrs+ETH_ALEN, peer->peer_mac, ETH_ALEN);
        }
        H_Init(&ctx, addrs, (ETH_ALEN * 2));
        H_Update(&ctx, (unsigned char *) grp->password, strlen(grp->password));
        H_Update(&ctx, &ctr, sizeof(ctr));
        H_Final(&ctx, pwe_digest);
        HMAC_CTX_cleanup(&ctx);

#if 0
        if (sae_debug_mask & SAE_DEBUG_CRYPTO_VERB) {
            if (memcmp(peer->peer_mac, peer->my_mac, ETH_ALEN) > 0) {
                printf("H(" MACSTR " | " MACSTR ", %s | %d)\n",
                       MAC2STR(peer->peer_mac), MAC2STR(peer->my_mac), grp->password, ctr);
            } else {
                printf("H(" MACSTR " | " MACSTR ", %s | %d)\n",
                       MAC2STR(peer->my_mac), MAC2STR(peer->peer_mac), grp->password, ctr);
            }
            dump_buffer(pwe_digest, SHA256_DIGEST_LENGTH);
        }
#endif

        BN_bin2bn(pwe_digest, SHA256_DIGEST_LENGTH, rnd);
        prf(pwe_digest, SHA256_DIGEST_LENGTH,
            (unsigned char *)"SAE Hunting and Pecking", strlen("SAE Hunting and Pecking"),
            primebuf, BN_num_bytes(grp->prime),
            prfbuf, primebitlen);
        BN_bin2bn(prfbuf, BN_num_bytes(grp->prime), x_candidate);
        /*
         * prf() returns a string of bits 0..primebitlen, but BN_bin2bn will
         * treat that string of bits as a big-endian number. If the primebitlen
         * is not an even multiple of 8 we masked off the excess bits-- those
         * _after_ primebitlen-- in prf() so now interpreting this as a
         * big-endian number is wrong. We have to shift right the amount we
         * masked off.
         */
        if (primebitlen % 8) {
            BN_rshift(x_candidate, x_candidate, (8 - (primebitlen % 8)));
        }

        /*
         * if this candidate value is greater than the prime then try again
         */
        if (BN_ucmp(x_candidate, grp->prime) >= 0) {
            continue;
        }

#if 0
        if (sae_debug_mask & SAE_DEBUG_CRYPTO_VERB) {
            memset(prfbuf, 0, BN_num_bytes(grp->prime));
            BN_bn2bin(x_candidate, prfbuf + (BN_num_bytes(grp->prime) - BN_num_bytes(x_candidate)));
            print_buffer("candidate x value", prfbuf, BN_num_bytes(grp->prime));
        }
#endif

        /*
         * compute y^2 using the equation of the curve
         *
         *      y^2 = x^3 + ax + b
         */
        BN_mod_sqr(tmp1, x_candidate, grp->prime, pSaeCtx->bnctx);
        BN_mod_mul(tmp2, tmp1, x_candidate, grp->prime, pSaeCtx->bnctx);
        BN_mod_mul(tmp1, a, x_candidate, grp->prime, pSaeCtx->bnctx);
        BN_mod_add_quick(tmp2, tmp2, tmp1, grp->prime);
        BN_mod_add_quick(tmp2, tmp2, b, grp->prime);

        /*
         * mask tmp2 so doing legendre won't leak timing info
         *
         * tmp1 is a random number between 1 and p-1
         */
        BN_rand_range(tmp1, pm1);

        BN_mod_mul(tmp2, tmp2, tmp1, grp->prime, pSaeCtx->bnctx);
        BN_mod_mul(tmp2, tmp2, tmp1, grp->prime, pSaeCtx->bnctx);
        /*
         * now tmp2 (y^2) is masked, all values between 1 and p-1
         * are equally probable. Multiplying by r^2 does not change
         * whether or not tmp2 is a quadratic residue, just masks it.
         *
         * flip a coin, multiply by the random quadratic residue or the
         * random quadratic nonresidue and record heads or tails
         */
        if (BN_is_odd(tmp1)) {
            BN_mod_mul(tmp2, tmp2, qr, grp->prime, pSaeCtx->bnctx);
            check = 1;
        } else {
            BN_mod_mul(tmp2, tmp2, qnr, grp->prime, pSaeCtx->bnctx);
            check = -1;
        }

        /*
         * now it's safe to do legendre, if check is 1 then it's
         * a straightforward test (multiplying by qr does not
         * change result), if check is -1 then its the opposite test
         * (multiplying a qr by qnr would make a qnr)
         */
        if (legendre(tmp2, grp->prime, pm1d2, pSaeCtx->bnctx) == check) {
            if (found == 1) {
                continue;
            }
            /*
             * need to unambiguously identify the solution, if there is one...
             */
            if (BN_is_odd(rnd)) {
                is_odd = 1;
            } else {
                is_odd = 0;
            }
            if ((x = BN_dup(x_candidate)) == NULL) {
                goto fail;
            }
            fprintf(stderr,  "it took %d tries to find PWE: %d\n", ctr, grp->group_num);
            found = 1;
        }
    }
    /*
     * 2^-40 is about one in a trillion so we should always find a point.
     * When we do, we know x^3 + ax + b is a quadratic residue so we can
     * assign a point using x and our discriminator (is_odd)
     */
    if ((found == 0) ||
        (!EC_POINT_set_compressed_coordinates_GFp(grp->group, peer->pwe, x, is_odd, pSaeCtx->bnctx))) {
        EC_POINT_free(peer->pwe);
        peer->pwe = NULL;
    }

#if 0
    if (found && peer->pwe && (sae_debug_mask & SAE_DEBUG_CRYPTO_VERB)) {
        BIGNUM *px = NULL, *py = NULL;
        if (((px = BN_new()) != NULL) &&
            ((py = BN_new()) != NULL)) {
            if (EC_POINT_get_affine_coordinates_GFp(peer->grp_def->group, peer->pwe, px, py, bnctx)) {
                printf("PWE (x,y):\n");
                memset(prfbuf, 0, BN_num_bytes(grp->prime));
                BN_bn2bin(px, prfbuf + (BN_num_bytes(grp->prime) - BN_num_bytes(px)));
                print_buffer("x", prfbuf, BN_num_bytes(grp->prime));
                memset(prfbuf, 0, BN_num_bytes(grp->prime));
                BN_bn2bin(py, prfbuf + (BN_num_bytes(grp->prime) - BN_num_bytes(py)));
                print_buffer("y", prfbuf, BN_num_bytes(grp->prime));
            }
            BN_free(px);
            BN_free(py);
        }
    }
#endif

fail:
    if (prfbuf != NULL) {
        free(prfbuf);
    }
    if (primebuf != NULL) {
        free(primebuf);
    }
    if (found) {
        BN_free(x);
    }

    BN_free(x_candidate);
    BN_free(rnd);
    BN_free(pm1d2);
    BN_free(pm1);
    BN_free(tmp1);
    BN_free(tmp2);
    BN_free(a);
    BN_free(b);
    BN_free(qr);
    BN_free(qnr);

    if (peer->pwe == NULL) {
        fprintf(stderr, "unable to find random point on curve for group %d, something's fishy!\n",
                  grp->group_num);
        return -1;
    }
    fprintf(stderr, "assigning group %d to peer, the size of the prime is %d\n",
              peer->grp_def->group_num, BN_num_bytes(peer->grp_def->prime));

    return 0;
}


int
commit_to_peer (P_SAE_CTX_T pSaeCtx, struct candidate *peer, unsigned char *token, int token_len)
{
    char buf[2048];
    struct ieee80211_mgmt_frame *frame;
    int offset1, offset2;
    size_t len = 0;
    BIGNUM *x, *y, *mask;
    unsigned short grp_num;
    unsigned char *ptr;

    memset(buf, 0, sizeof(buf));
    frame = (struct ieee80211_mgmt_frame *)buf;

    /*
     * fill in authentication frame header...
     */
    frame->frame_control = ieee_order((IEEE802_11_FC_TYPE_MGMT << 2 | IEEE802_11_FC_STYPE_AUTH << 4));
    memcpy(frame->sa, peer->my_mac, ETH_ALEN);
    memcpy(frame->da, peer->peer_mac, ETH_ALEN);
    memcpy(frame->bssid, peer->peer_mac, ETH_ALEN);

    frame->authenticate.alg = ieee_order(SAE_AUTH_ALG);
    frame->authenticate.auth_seq = ieee_order(SAE_AUTH_COMMIT);
    len = IEEE802_11_HDR_LEN + sizeof(frame->authenticate);
    ptr = frame->authenticate.u.var8;

    /*
     * first, indicate what group we're committing with
     */
    grp_num = ieee_order(peer->grp_def->group_num);
    memcpy(ptr, &grp_num, sizeof(unsigned short));
    ptr += sizeof(unsigned short);
    len += sizeof(unsigned short);

    /*
     * if we've been asked to include a token then include a token
     */
    if (token_len && (token != NULL)) {
        memcpy(ptr, token, token_len);
        ptr += token_len;
        len += token_len;
    }

    if (peer->private_val == NULL) {
        if (((mask = BN_new()) == NULL) ||
            ((peer->private_val = BN_new()) == NULL)) {
            fprintf(stderr, "unable to commit to peer!\n");
            return -1;
        }
        /*
         * generate private values
         */
        BN_rand_range(peer->private_val, peer->grp_def->order);
        BN_rand_range(mask, peer->grp_def->order);
#if 0
        if (sae_debug_mask & SAE_DEBUG_CRYPTO_VERB) {
            pp_a_bignum("local private value", peer->private_val);
            pp_a_bignum("local mask value", mask);
        }
#endif
        /*
         * generate scalar = (priv + mask) mod order
         */
        BN_add(peer->my_scalar, peer->private_val, mask);
        BN_mod(peer->my_scalar, peer->my_scalar, peer->grp_def->order, pSaeCtx->bnctx);
        /*
         * generate element = -(mask*pwe)
         */
        if (!EC_POINT_mul(peer->grp_def->group, peer->my_element, NULL, peer->pwe, mask, pSaeCtx->bnctx)) {
            fprintf(stderr, "unable to compute A!\n");
            BN_free(mask);
            return -1;
        }
        if (!EC_POINT_invert(peer->grp_def->group, peer->my_element, pSaeCtx->bnctx)) {
            fprintf(stderr, "unable to invert A!\n");
            BN_free(mask);
            return -1;
        }
        BN_free(mask);
    }
    if (((x = BN_new()) == NULL) ||
        ((y = BN_new()) == NULL)) {
        fprintf(stderr, "unable to create x,y bignums\n");
        return -1;
    }
    if (!EC_POINT_get_affine_coordinates_GFp(peer->grp_def->group, peer->my_element, x, y, pSaeCtx->bnctx)) {
        fprintf(stderr, "unable to get secret key!\n");
        BN_free(x);
        BN_free(y);
        return -1;
    }
#if 0
    if (sae_debug_mask & SAE_DEBUG_CRYPTO_VERB) {
        printf("local commit:\n");
        pp_a_bignum("my scalar", peer->my_scalar);
        printf("my element:\n");
        pp_a_bignum("x", x);
        pp_a_bignum("y", y);
    }
#endif
    /*
     * fill in the commit, first in the commit message is the scalar
     */
    offset1 = BN_num_bytes(peer->grp_def->order) - BN_num_bytes(peer->my_scalar);
    BN_bn2bin(peer->my_scalar, ptr + offset1);
    ptr += BN_num_bytes(peer->grp_def->order);
    len += BN_num_bytes(peer->grp_def->order);

    /*
     * ...next is the element, x then y
     */
    if (!EC_POINT_get_affine_coordinates_GFp(peer->grp_def->group, peer->my_element, x, y, pSaeCtx->bnctx)) {
        fprintf(stderr, "unable to determine u!\n");
        exit(1);
    }
    offset1 = BN_num_bytes(peer->grp_def->prime) - BN_num_bytes(x);
    BN_bn2bin(x, ptr + offset1);
    ptr += BN_num_bytes(peer->grp_def->prime);

    offset2 = BN_num_bytes(peer->grp_def->prime) - BN_num_bytes(y);
    BN_bn2bin(y, ptr + offset2);
    ptr += BN_num_bytes(peer->grp_def->prime);

    len += (2 * BN_num_bytes(peer->grp_def->prime));

    fprintf(stderr, "peer " MACSTR " in %s, sending %s (%s token), len %d, group %d\n",
              MAC2STR(peer->peer_mac),
              state_to_string(peer->state),
              seq_to_string(ieee_order(frame->authenticate.auth_seq)),
              (token_len ? "with" : "no"), (int)len,
              peer->grp_def->group_num);
    BN_free(x);
    BN_free(y);

    //++ CowTwo: dump frame content
    fprintf(stderr, "frame=[");
    {
        int ii;
        unsigned char* dumpPtr= (unsigned char*)frame;
        for (ii=0;ii<len;ii++){
            fprintf(stderr, "0x%02x", (unsigned char)(*dumpPtr));
            if (ii<(len-1))
                fprintf(stderr, ", ");
            else
                fprintf(stderr, "];");
            if (ii%8==7)
                fprintf(stderr,"\n       ");
            dumpPtr+=1;
        }
    }
    //--

#if 1
    if (meshd_write_mgmt(pSaeCtx, buf, len, peer->cookie) != len) {
        fprintf(stderr, "can't send an authentication frame to " MACSTR "\n",
                  MAC2STR(peer->peer_mac));
        return -1;
    }
#endif

    return 0;
}

int
process_commit (P_SAE_CTX_T pSaeCtx, struct candidate *peer, struct ieee80211_mgmt_frame *frame, int len)
{
    BIGNUM *x, *y, *k, *nsum;
    int offset, itemsize, ret = 0;
    EC_POINT *K;
    unsigned char *ptr, *tmp, keyseed[SHA256_DIGEST_LENGTH], kckpmk[(SHA256_DIGEST_LENGTH * 2) * 8];
    HMAC_CTX ctx;

    /*
     * check whether the frame is big enough (might be proprietary IEs or cruft appended)
     */
    if (len < (IEEE802_11_HDR_LEN + sizeof(frame->authenticate) +
                (2 * BN_num_bytes(peer->grp_def->prime)) + BN_num_bytes(peer->grp_def->order))) {
        fprintf(stderr,"invalid size for commit message (%d < %d+%d+(2*%d)+%d = %d))\n", len,
                  IEEE802_11_HDR_LEN, (int)sizeof(frame->authenticate), BN_num_bytes(peer->grp_def->prime),
                  BN_num_bytes(peer->grp_def->order),
                  (int)(IEEE802_11_HDR_LEN+sizeof(frame->authenticate)+
                   (2*BN_num_bytes(peer->grp_def->prime)) + BN_num_bytes(peer->grp_def->order)));
        return -1;
    }
    if (((x = BN_new()) == NULL) ||
        ((y = BN_new()) == NULL) ||
        ((k = BN_new()) == NULL) ||
        ((K = EC_POINT_new(peer->grp_def->group)) == NULL)) {
        fprintf(stderr, "unable to create x,y bignums\n");
        return -1;
    }
    ptr = frame->authenticate.u.var8;
    /*
     * first thing in a commit is the finite cyclic group, skip the group
     */
    ptr += sizeof(unsigned short);

    if (peer->got_token) {
        /*
         * if we got a token then skip over it. We know the size because we
         * created it in the first place!
         */
        ptr += SHA256_DIGEST_LENGTH;
    }

    /*
     * first get the peer's scalar
     */
    itemsize = BN_num_bytes(peer->grp_def->order);
    BN_bin2bn(ptr, itemsize, peer->peer_scalar);
    ptr += itemsize;
    /*
     * then get x and y and turn them into the peer's element
     */
    itemsize = BN_num_bytes(peer->grp_def->prime);
    BN_bin2bn(ptr, itemsize, x);
    ptr += itemsize;
    BN_bin2bn(ptr, itemsize, y);

    if (!EC_POINT_set_affine_coordinates_GFp(peer->grp_def->group, peer->peer_element, x, y, pSaeCtx->bnctx)) {
        fprintf(stderr, "unable to obtain peer's password element!\n");
        goto fail;
    }

    /*
     * validate the scalar...
     */
    if ((BN_cmp(peer->peer_scalar, BN_value_one()) < 1) ||
        (BN_cmp(peer->peer_scalar, peer->grp_def->order) > 0)) {
        fprintf(stderr, "peer's scalar is invalid!\n");
        goto fail;
    }
    /*
     * ...and the element
     */
    if (!EC_POINT_is_on_curve(peer->grp_def->group, peer->peer_element, pSaeCtx->bnctx)) {
        fprintf(stderr, "peer's element is invalid!\n");
        goto fail;
    }

#if 0
    if (sae_debug_mask & SAE_DEBUG_CRYPTO_VERB) {
        printf("peer's commit:\n");
        pp_a_bignum("peer's scalar", peer->peer_scalar);
        printf("peer's element:\n");
        pp_a_bignum("x", x);
        pp_a_bignum("y", y);
    }
#endif

    /*
     * now compute: scalar * PWE...
     */
    if (!EC_POINT_mul(peer->grp_def->group, K, NULL, peer->pwe, peer->peer_scalar, pSaeCtx->bnctx)) {
        fprintf(stderr, "unable to multiply peer's scalar and PWE!\n");
        goto fail;
    }

    /*
     * ... + element
     */
    if (!EC_POINT_add(peer->grp_def->group, K, K, peer->peer_element, pSaeCtx->bnctx)) {
        fprintf(stderr, "unable to add element to running point!\n");
        goto fail;
    }
    /*
     * ... * private val = our private_val * peer's private_val * pwe
     */
    if (!EC_POINT_mul(peer->grp_def->group, K, NULL, K, peer->private_val, pSaeCtx->bnctx)) {
        fprintf(stderr, "unable to multiple intermediate by private value!\n");
        goto fail;
    }

    if (!EC_POINT_get_affine_coordinates_GFp(peer->grp_def->group, K, k, NULL, pSaeCtx->bnctx)) {
        fprintf(stderr, "unable to get secret key!\n");
        goto fail;
    }

    /*
     * compute the KCK and PMK
     */
    if ((tmp = (unsigned char *)malloc(BN_num_bytes(peer->grp_def->prime))) == NULL) {
        fprintf(stderr, "unable to malloc %d bytes for secret!\n",
                  BN_num_bytes(k));
        goto fail;
    }
    /*
     * first extract the entropy from k into keyseed...
     */
    offset = BN_num_bytes(peer->grp_def->prime) - BN_num_bytes(k);
    memset(tmp, 0, offset);
    BN_bn2bin(k, tmp + offset);
    H_Init(&ctx, pSaeCtx->allzero, SHA256_DIGEST_LENGTH);
    H_Update(&ctx, tmp, BN_num_bytes(peer->grp_def->prime));
    H_Final(&ctx, keyseed);
    HMAC_CTX_cleanup(&ctx);
    free(tmp);

    /*
     * ...then expand it to create KCK | PMK
     */
    if (((tmp = (unsigned char *)malloc(BN_num_bytes(peer->grp_def->order))) == NULL) ||
        ((nsum = BN_new()) == NULL)) {
        fprintf(stderr, "unable to create buf/bignum to sum scalars!\n");
        goto fail;
    }
    BN_add(nsum, peer->my_scalar, peer->peer_scalar);
    BN_mod(nsum, nsum, peer->grp_def->order, pSaeCtx->bnctx);
    offset = BN_num_bytes(peer->grp_def->order) - BN_num_bytes(nsum);
    memset(tmp, 0, offset);
    BN_bn2bin(nsum, tmp + offset);

    memcpy(peer->pmkid, tmp, 16);

    prf(keyseed, SHA256_DIGEST_LENGTH,
        (unsigned char *)"SAE KCK and PMK", strlen("SAE KCK and PMK"),
        tmp, BN_num_bytes(peer->grp_def->order),
        kckpmk, ((SHA256_DIGEST_LENGTH * 2) * 8));
    free(tmp);
    BN_free(nsum);

    memcpy(peer->kck, kckpmk, SHA256_DIGEST_LENGTH);
    memcpy(peer->pmk, kckpmk+SHA256_DIGEST_LENGTH, SHA256_DIGEST_LENGTH);

#if 1
    if (1/*sae_debug_mask & SAE_DEBUG_CRYPTO_VERB*/) {
        pp_a_bignum((char*)"k", k);
        print_buffer((char*)"keyseed", keyseed, SHA256_DIGEST_LENGTH);
        print_buffer((char*)"KCK", peer->kck, SHA256_DIGEST_LENGTH);
        print_buffer((char*)"PMK", peer->pmk, SHA256_DIGEST_LENGTH);
    }
#endif
    if (0) {
fail:
        ret = -1;
    }
    BN_free(x);
    BN_free(y);
    BN_free(k);
    EC_POINT_free(K);

    return ret;
}

int
confirm_to_peer (P_SAE_CTX_T pSaeCtx, struct candidate *peer)
{
    char buf[2048];
    unsigned char tmp[128];
    struct ieee80211_mgmt_frame *frame;
    size_t len = 0;
    BIGNUM *x, *y;
    HMAC_CTX ctx;
    unsigned short send_conf;
    int offset;

    if (((x = BN_new()) == NULL) ||
        ((y = BN_new()) == NULL)) {
        fprintf(stderr, "unable to construct confirm!\n");
        return -1;
    }

    memset(buf, 0, sizeof(buf));
    frame = (struct ieee80211_mgmt_frame *)buf;

    frame->frame_control = ieee_order((IEEE802_11_FC_TYPE_MGMT << 2 | IEEE802_11_FC_STYPE_AUTH << 4));
    memcpy(frame->sa, peer->my_mac, ETH_ALEN);
    memcpy(frame->da, peer->peer_mac, ETH_ALEN);
    memcpy(frame->bssid, peer->peer_mac, ETH_ALEN);

    frame->authenticate.alg = ieee_order(SAE_AUTH_ALG);
    frame->authenticate.auth_seq = ieee_order(SAE_AUTH_CONFIRM);
    len = IEEE802_11_HDR_LEN + sizeof(frame->authenticate);

    if (peer->sc != COUNTER_INFINITY) {
        peer->sc++;
    }
    send_conf = ieee_order(peer->sc);
    memcpy(frame->authenticate.u.var8, (unsigned char *)&send_conf, sizeof(unsigned short));
    len += sizeof(unsigned short);


    CN_Init(&ctx, peer->kck, SHA256_DIGEST_LENGTH);     /* the key */

    /* send_conf is in over-the-air format now */
    CN_Update(&ctx, (unsigned char *)&send_conf, sizeof(unsigned short));

        /* my scalar */
    offset = BN_num_bytes(peer->grp_def->order) - BN_num_bytes(peer->my_scalar);
    memset(tmp, 0, offset);
    BN_bn2bin(peer->my_scalar, tmp + offset);
    CN_Update(&ctx, tmp, BN_num_bytes(peer->grp_def->order));

    if (!EC_POINT_get_affine_coordinates_GFp(peer->grp_def->group, peer->my_element, x, y, pSaeCtx->bnctx)) {
        fprintf(stderr, "unable to get x,y of my element\n");
        BN_free(x);
        BN_free(y);
        return -1;
    }
        /* my element */
    offset = BN_num_bytes(peer->grp_def->prime) - BN_num_bytes(x);
    memset(tmp, 0, offset);
    BN_bn2bin(x, tmp + offset);
    CN_Update(&ctx, tmp, BN_num_bytes(peer->grp_def->prime));
    offset = BN_num_bytes(peer->grp_def->prime) - BN_num_bytes(y);
    memset(tmp, 0, offset);
    BN_bn2bin(y, tmp + offset);
    CN_Update(&ctx, tmp, BN_num_bytes(peer->grp_def->prime));

        /* peer's scalar */
    offset = BN_num_bytes(peer->grp_def->order) - BN_num_bytes(peer->peer_scalar);
    memset(tmp, 0, offset);
    BN_bn2bin(peer->peer_scalar, tmp + offset);
    CN_Update(&ctx, tmp, BN_num_bytes(peer->grp_def->order));

    if (!EC_POINT_get_affine_coordinates_GFp(peer->grp_def->group, peer->peer_element, x, y, pSaeCtx->bnctx)) {
        fprintf(stderr, "unable to get x,y of peer's element\n");
        BN_free(x);
        BN_free(y);
        return -1;
    }
        /* peer's element */
    offset = BN_num_bytes(peer->grp_def->prime) - BN_num_bytes(x);
    memset(tmp, 0, offset);
    BN_bn2bin(x, tmp + offset);
    CN_Update(&ctx, tmp, BN_num_bytes(peer->grp_def->prime));
    offset = BN_num_bytes(peer->grp_def->prime) - BN_num_bytes(y);
    memset(tmp, 0, offset);
    BN_bn2bin(y, tmp + offset);
    CN_Update(&ctx, tmp, BN_num_bytes(peer->grp_def->prime));

    CN_Final(&ctx, (frame->authenticate.u.var8 + sizeof(unsigned short)));
    HMAC_CTX_cleanup(&ctx);

    if (1/*sae_debug_mask & SAE_DEBUG_CRYPTO_VERB*/) {
        print_buffer((char*)"local confirm",
                    frame->authenticate.u.var8,
                    SHA256_DIGEST_LENGTH + sizeof(unsigned short));
    }

    len += SHA256_DIGEST_LENGTH;

    fprintf(stderr, MACSTR " in %s, sending %s (sc=%d), len %d\n",
              MAC2STR(peer->peer_mac),
              state_to_string(peer->state),
              seq_to_string(ieee_order(frame->authenticate.auth_seq)),
              peer->sc, (int)len);
#if 1
    if (meshd_write_mgmt(pSaeCtx, buf, len, peer->cookie) != len) {
        fprintf(stderr, "can't send an authentication frame to " MACSTR "\n",
                MAC2STR(peer->peer_mac));
        return -1;
    }
#endif
    BN_free(x);
    BN_free(y);

    return 0;
}

int
process_confirm (P_SAE_CTX_T pSaeCtx, struct candidate *peer,
        struct ieee80211_mgmt_frame *frame, int len)
{
    unsigned char tmp[128];
    enum result r = NO_ERR;
    BIGNUM *x = NULL;
    BIGNUM *y = NULL;
    EC_POINT *psum = NULL;
    HMAC_CTX ctx;
    int offset;

    if (len != (IEEE802_11_HDR_LEN + sizeof(frame->authenticate) + sizeof(unsigned short) + SHA256_DIGEST_LENGTH)) {
        fprintf(stderr, "bad size of confirm message (%d)\n", len);
        r = ERR_NOT_FATAL;
        goto out;
    }
    if (((x = BN_new()) == NULL) ||
        ((y = BN_new()) == NULL) ||
        ((psum = EC_POINT_new(peer->grp_def->group)) == NULL)) {
        fprintf(stderr, "unable to construct confirm!\n");
        r = ERR_FATAL;
        goto out;
    }

    CN_Init(&ctx, peer->kck, SHA256_DIGEST_LENGTH);     /* the key */

    peer->rc = ieee_order(*(frame->authenticate.u.var16));
    fprintf(stderr, "processing confirm (%d)\n", peer->rc);
    /*
     * compute the confirm verifier using the over-the-air format of send_conf
     */
    CN_Update(&ctx, frame->authenticate.u.var8, sizeof(unsigned short));

        /* peer's scalar */
    offset = BN_num_bytes(peer->grp_def->order) - BN_num_bytes(peer->peer_scalar);
    memset(tmp, 0, offset);
    BN_bn2bin(peer->peer_scalar, tmp + offset);
    CN_Update(&ctx, tmp, BN_num_bytes(peer->grp_def->order));

    if (!EC_POINT_get_affine_coordinates_GFp(peer->grp_def->group, peer->peer_element, x, y, pSaeCtx->bnctx)) {
        fprintf(stderr, "unable to get x,y of peer's element\n");
        r = ERR_NOT_FATAL;
        goto out;
    }

    /* Rarely x can be way too big, e.g. 1348 bytes. Corrupted packet? */
    if (BN_num_bytes(peer->grp_def->prime) < BN_num_bytes(x) || BN_num_bytes(peer->grp_def->prime) < BN_num_bytes(y)) {
        fprintf(stderr, "coords are too big, x = %d bytes, y = %d bytes\n", BN_num_bytes(x), BN_num_bytes(y));
        r = ERR_NOT_FATAL;
        goto out;
    }

        /* peer's element */
    offset = BN_num_bytes(peer->grp_def->prime) - BN_num_bytes(x);
    memset(tmp, 0, offset);
    BN_bn2bin(x, tmp + offset);
    CN_Update(&ctx, tmp, BN_num_bytes(peer->grp_def->prime));
    offset = BN_num_bytes(peer->grp_def->prime) - BN_num_bytes(y);
    memset(tmp, 0, offset);
    BN_bn2bin(y, tmp + offset);
    CN_Update(&ctx, tmp, BN_num_bytes(peer->grp_def->prime));

        /* my scalar */
    offset = BN_num_bytes(peer->grp_def->order) - BN_num_bytes(peer->my_scalar);
    memset(tmp, 0, offset);
    BN_bn2bin(peer->my_scalar, tmp + offset);
    CN_Update(&ctx, tmp, BN_num_bytes(peer->grp_def->order));

    if (!EC_POINT_get_affine_coordinates_GFp(peer->grp_def->group, peer->my_element, x, y, pSaeCtx->bnctx)) {
        fprintf(stderr, "unable to get x,y of my element\n");
        r = ERR_NOT_FATAL;
        goto out;
    }
        /* my element */
    offset = BN_num_bytes(peer->grp_def->prime) - BN_num_bytes(x);
    memset(tmp, 0, offset);
    BN_bn2bin(x, tmp + offset);
    CN_Update(&ctx, tmp, BN_num_bytes(peer->grp_def->prime));
    offset = BN_num_bytes(peer->grp_def->prime) - BN_num_bytes(y);
    memset(tmp, 0, offset);
    BN_bn2bin(y, tmp + offset);
    CN_Update(&ctx, tmp, BN_num_bytes(peer->grp_def->prime));

    CN_Final(&ctx, tmp);
    HMAC_CTX_cleanup(&ctx);

    if (1/*sae_debug_mask & SAE_DEBUG_CRYPTO_VERB*/) {
        print_buffer((char*)"peer's confirm",
                    frame->authenticate.u.var8,
                    SHA256_DIGEST_LENGTH + sizeof(unsigned short));
    }

    if (memcmp(tmp, (frame->authenticate.u.var8 + sizeof(unsigned short)), SHA256_DIGEST_LENGTH)) {
        fprintf(stderr, "confirm did not verify!\n");
        r = ERR_BLACKLIST;
        goto out;
    }

    r = NO_ERR;

out:
    BN_free(x);
    BN_free(y);
    EC_POINT_free(psum);

    return r;
}


