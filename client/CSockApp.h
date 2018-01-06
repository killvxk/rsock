//
// Created by System Administrator on 12/26/17.
//

#ifndef RSOCK_CSOCKAPP_H
#define RSOCK_CSOCKAPP_H


#include "../ISockApp.h"

class CSockApp : public ISockApp {
public:
    CSockApp(uv_loop_t *loop);

    RCap *CreateCap(RConfig &conf) override;

    IRawConn *CreateBtmConn(RConfig &conf, uv_loop_t *loop, int datalink, int conn_type) override;

    IConn *CreateBridgeConn(RConfig &conf, IRawConn *btm, uv_loop_t *loop, SockMon *mon) override;

    SockMon *InitSockMon(uv_loop_t *loop, const RConfig &conf) override;
};


#endif //RSOCK_CSOCKAPP_H
