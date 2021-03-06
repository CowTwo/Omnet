/*
 * other_if.cc
 *
 *  Created on: Dec 30, 2017
 *      Author: root
 */


#include "Txc.h"

z_ct_sae::Txc* simGetModuleObjectFromSaeCtx(P_SAE_CTX_T pSaeCtx)

{
    return ((z_ct_sae::Txc*)(pSaeCtx)->pSimModuleObject);

}


int meshd_write_mgmt(P_SAE_CTX_T pSaeCtx, char *buf, int framelen, void *cookie)
{
    unsigned char *frameBuf= (unsigned char*)malloc(framelen);
    memcpy(frameBuf, buf, framelen);

    cMessage *msg = new cMessage("tictocMsg");
    msg->setContextPointer(frameBuf);
    msg->setKind(framelen);

    simGetModuleObjectFromSaeCtx(pSaeCtx)->send(msg, "out");

    return framelen;
}

