//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
// 
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/.
// 

#include "Txc.h"
#include "ieee802_11.h"
#include "gl_typedef.h"

namespace z_ct_sae {

Define_Module(Txc);


void Txc::initialize()
{
    rSaeCtx.bnctx=NULL;
    if (env_init(&rSaeCtx)!=1){
        ASSERT(0);
    }

    if (par("sendInitialMessage").boolValue())
    {
        rSaeCtx.pSimModuleObject=this;
        //cMessage *msg = new cMessage("tictocMsg");

        //++
        {
            unsigned char l_myMac[ETH_ALEN]={0x10,0x20,0x30,0x00,0x00,0x00};
            unsigned char l_herMac[ETH_ALEN]={0x40,0x50,0x60,0x00,0x00,0x00};
            memcpy(rSaeCtx.myMac, l_myMac, ETH_ALEN);
            memcpy(rSaeCtx.herMac, l_herMac, ETH_ALEN);

            saeInitiateCommit2Peer(&rSaeCtx);
        }
#if 0
        msg->setContextPointer(rSaeCtx.frameBuf);
        msg->setKind(rSaeCtx.frmeLen);
        //--

        send(msg, "out");
#endif
    }
    else{
        rSaeCtx.pSimModuleObject=this;
        //++
        {
            unsigned char l_myMac[ETH_ALEN]={0x40,0x50,0x60,0x00,0x00,0x00};
            unsigned char l_herMac[ETH_ALEN]={0x10,0x20,0x30,0x00,0x00,0x00};
            memcpy(rSaeCtx.myMac, l_myMac, ETH_ALEN);
            memcpy(rSaeCtx.herMac, l_herMac, ETH_ALEN);
        }
        //--

    }
}

void Txc::handleMessage(cMessage *msg)
{
    struct ieee80211_mgmt_frame *frame;
    int len;

    frame=(ieee80211_mgmt_frame *)msg->getContextPointer();
    len=(int)msg->getKind();

    process_mgmt_frame(&rSaeCtx, frame, len, rSaeCtx.myMac, (void*)0);

    // just send back the message we received
    //send(msg, "out");
}


}; // namespace
