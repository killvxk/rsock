//
// Created on 12/17/17.
//

#include <cassert>
#include <plog/Log.h>
#include "IRawConn.h"
#include "cap/cap_headers.h"
#include "util/rsutil.h"
#include "OHead.h"
#include "util/enc.h"
#include "util/rhash.h"

// todo: change src and dst to self and target.
// RawConn has key of nullptr to expose errors as fast as it can if any.
IRawConn::IRawConn(const std::string &dev, IUINT32 selfInt, uv_loop_t *loop, const std::string &hashKey, bool is_server,
                   int type, int datalinkType, IUINT32 targetInt)
        : IConn("IRawConn"), mSelf(selfInt), mLoop(loop), mHashKey(hashKey),
          mIsServer(is_server), mConnType(type), mDatalink(datalinkType), mTarget(targetInt), mDev(dev) {
    assert(loop != nullptr);
    mSelfNetEndian = mSelf;
    mTargetNetEndian = mTarget;
    assert(mSelf != 0);
}

int IRawConn::Init() {
    IConn::Init();
    char err[LIBNET_ERRBUF_SIZE] = {0};
    mTcpNet = libnet_init(LIBNET_RAW4, mDev.c_str(), err);
    if (!mTcpNet) {
        LOGE << "failed to init libnet: " << err;
        return -1;
    }
    mUdpNet = libnet_init(LIBNET_RAW4, mDev.c_str(), err);
    if (!mUdpNet) {
        LOGE << "failed to init libnet: " << err;
        return -1;
    }

    int nret = socketpair(AF_UNIX, SOCK_DGRAM, 0, mSockPair);
    if (nret) {
        LOGE << "failed to create unix socket pair: " << strerror(errno);
        return nret;
    }

    in_addr src = {mSelfNetEndian};
    const char *s = inet_ntoa(src);
    if (!s) {
        LOGE << "inet_ntoa failed: " << strerror(errno);
        return -1;
    }
    memcpy(mSelfStr, s, strlen(s));
    in_addr target = {mTargetNetEndian};
    s = inet_ntoa(target);
    if (!s) {
        LOGE << "inet_ntoa failed: " << strerror(errno);
        return -1;
    }
    memcpy(mTargetStr, s, strlen(s));

    mReadFd = mSockPair[0];
    mWriteFd = mSockPair[1];
    mUnixDgramPoll = poll_dgram_fd(mReadFd, mLoop, pollCb, this, &nret);
    if (!mUnixDgramPoll) {
        LOGE << "poll failed: %s" << uv_strerror(nret);
        return nret;
    }
    return 0;
}

int IRawConn::Output(ssize_t nread, const rbuf_t &rbuf) {
    if (nread > 0) {
        OHead *oh = static_cast<OHead *>(rbuf.data);
#ifndef NNDEBUG
        assert(oh != nullptr);
#else
        if (!oh) {
            return 0;
        }
#endif
        const int ENC_SIZE = oh->GetEncBufSize();

        if (HASH_BUF_SIZE + ENC_SIZE + nread > OM_MAX_PKT_SIZE) {
            LOGE << "packet exceeds MTU. redefine MTU. MTU: " << OM_MAX_PKT_SIZE << ", HASH_BUF_SIZE: " << HASH_BUF_SIZE
                 << ", ENC_SIZE: " << ENC_SIZE << ", nread: " << nread;
#ifndef NNDEBUG
            assert(HASH_BUF_SIZE + ENC_SIZE + nread <= OM_MAX_PKT_SIZE);
#else
            return -1;
#endif
        }

        // todo: make OM_MAX_PKT_SIZE configurable, rather than macro
        IUINT8 buf[OM_MAX_PKT_SIZE] = {0};
        compute_hash((char *) buf, mHashKey, rbuf.base, nread);
        IUINT8 *p = buf + HASH_BUF_SIZE;
        p = oh->Enc2Buf(p, sizeof(buf) - HASH_BUF_SIZE);
        if (!p) {
#ifndef NNDEBUG
            LOGE << "OHead failed to encode buf.";
            assert(0);
#else
            return 0;
#endif
        }

        memcpy(p, rbuf.base, nread);
        p += nread;

        IUINT16 sp = oh->SourcePort();
        IUINT16 dp = oh->DstPort();
        assert(dp != 0);
        assert(sp != 0);
        IUINT16 len = p - buf;
        IUINT32 seq = oh->IncSeq(len);
        IUINT32 ack = oh->Ack();
        IUINT16 ipid = mIpId++;
        IUINT32 dst = oh->Dst();
        int conn_type = mIsServer ? oh->ConnType() : mConnType;
        if (!conn_type) {
            conn_type = OM_PIPE_TCP_SEND;
        }

        IUINT8 flag = TH_ACK;
//        if (ack <= OM_INIT_ACK_SYN) {
//            if (mIsServer) {
//                flag = TH_SYN | TH_ACK;
//            } else {
//                flag = TH_SYN;
//            }
//        }

        if (conn_type & OM_PIPE_TCP_SEND) {
            return SendRawTcp(mTcpNet, mSelf, sp, dst, dp, seq, ack, buf, len, ipid, mTcp, mIpForTcp, flag);
        } else {
            return SendRawUdp(mUdpNet, mSelf, sp, dst, dp, buf, len, ipid, mUdp, mIpForUdp);
        }
    }

    return nread;
}

void IRawConn::CapInputCb(u_char *args, const pcap_pkthdr *hdr, const u_char *packet) {
    IRawConn *conn = reinterpret_cast<IRawConn *>(args);
    conn->RawInput(args, hdr, packet);
}

// todo: 本地向服务器发送数据的时候，本地会再次接收到数据
int IRawConn::RawInput(u_char *args, const pcap_pkthdr *hdr, const u_char *packet) {
    struct ip *ip = NULL;
    if (mDatalink == DLT_EN10MB) {    // ethernet
        struct oetherhdr *eth = (struct oetherhdr *) packet;
        if (eth->ether_type != OM_PROTO_IP) {
            LOGE << "ethernet. only ipv4 protocol is supported. proto: " << eth->ether_type;
            return 0;
        }

        ip = (struct ip *) (packet + LIBNET_ETH_H);
    } else if (mDatalink == DLT_NULL) {   // loopback
        IUINT32 type = 0;
        decode_uint32(&type, reinterpret_cast<const char *>(packet));
        // the link layer header is a 4-byte field, in host byte order, containing a value of 2 for IPv4 packets
        // https://www.tcpdump.org/linktypes.html
        if (2 != type) {
            LOGE << "loopback. only ipv4 protocol is supported. proto: " << type;
            return 0;
        }
        ip = (struct ip *) (packet + 4);
    } else {
        LOGE << "unsupported datalink type: " << mDatalink;
#ifndef NNDEBUG
        assert(0);
#else
        return 0;
#endif
    }

    if (ip->ip_dst.s_addr != mSelfNetEndian) {
        LOGE << "dst is not this machine";
        return 0;
    }

    if (!mIsServer) {    // meanless to check src for server
        if (ip->ip_src.s_addr != mTargetNetEndian) {
            LOGE << "client: src is not target.";
            return 0;
        }
    }

    const int proto = ip->ip_p;

    if (ip->ip_hl != (LIBNET_IPV4_H >> 2)) {    // header len must be 20
        LOGE << "ip header len doesn't equal to " << LIBNET_IPV4_H;
        return 0;
    }

    if (proto != IPPROTO_TCP && proto != IPPROTO_UDP) {
        LOGE << "only tcp/udp are supported. proto: " << proto;
        return 0;
    }


    in_port_t src_port = 0;  // network endian
    in_port_t dst_port = 0;
    const char *hashhead = nullptr;

    CMD_TYPE cmd = 0;
    IUINT32 ackForPeer = 0;
    if (proto == IPPROTO_TCP) {
        // todo: remove check. pcap filter it??
        if (!(mConnType & OM_PIPE_TCP_RECV)) {  // check incomming packets
            LOGE << "conn type " << mConnType << ", but receive tcp packet";
            return 0;
        }

        struct tcphdr *tcp = (struct tcphdr *) ((const char *) ip + LIBNET_IPV4_H);
        src_port = (tcp->th_sport);
        dst_port = tcp->th_dport;
        hashhead = (const char *) tcp + (tcp->th_off << 2);
        cmd = CMD_TCP;
        ackForPeer = ntohl(tcp->th_seq) + (hdr->len - ((const u_char *) hashhead - packet));
        if (tcp->th_flags & TH_SYN) {
            ackForPeer = mIsServer? OM_INIT_ACK: OM_INIT_ACK_SYN;
        }
//        IUINT8 flag = 0;
        std::string flag = "";
        if (tcp->th_flags & TH_SYN) {
            flag += "SYN|";
        }
        if (tcp->th_flags & TH_ACK) {
            flag += "ACK|";
        }
        if (tcp->th_flags & TH_PUSH) {
            flag += "PUSH|";
        }
        if (tcp->th_flags & TH_RST) {
            flag += "RST|";
        }
        if (tcp->th_flags & TH_FIN) {
            flag += "FIN";
        }
        LOGV << "flag: " << flag;
    } else if (proto == IPPROTO_UDP) {
        if (!(mConnType & OM_PIPE_UDP_RECV)) {  // check incomming packets
            LOGE << "conn type " << mConnType << ", but receive udp packet" << mConnType;
            return 0;
        }

        struct udphdr *udp = (struct udphdr *) ((const char *) ip + LIBNET_IPV4_H);
        src_port = (udp->uh_sport);
        dst_port = udp->uh_dport;
        hashhead = (const char *) udp + LIBNET_UDP_H;
        cmd = CMD_UDP;
    }

//    const int len = hdr->len - ((const u_char *) hashhead - packet);
    const int lenWithHash = ntohs(ip->ip_len) - ((const u_char *) hashhead - (const u_char *) ip);
    // this check is necessary.
    // because we may receive rst with length zero. if we don't check, we may cause illegal memory access error

    if (mIsServer) {    // if server, target str is not computed
        in_addr src_in_addr = {ip->ip_src};
        const char *src = inet_ntoa(src_in_addr);
        LOGV << "receive " << lenWithHash << " bytes from " << src << ":" << ntohs(src_port) << "<->" << mSelfStr << ":"
             << ntohs(dst_port);
    } else {
        LOGV << "receive " << lenWithHash << " bytes from " << mTargetStr << ":" << ntohs(src_port) << "<->" << mSelfStr
             << ":" << ntohs(dst_port);
    }

    if (hdr->len - ((const u_char *) hashhead - packet) < HASH_BUF_SIZE + 1) {  // data len must >= 1
        return 0;
    }

    const char *ohead = hashhead + HASH_BUF_SIZE;
    IUINT8 oheadLen = 0;
    decode_uint8(&oheadLen, ohead);
    if (oheadLen != OHead::GetEncBufSize()) {
        LOGE << "failed to decode len. decoded len: " << oheadLen << ", fixed EncBufSize: " << OHead::GetEncBufSize();
        return 0;
    }

    const char *data = ohead + oheadLen;
    int data_len = lenWithHash - (data - hashhead);
    if (data_len <= 0) {
        LOGE << "data_len <= 0! data_len: " << data_len << ", hdr.len: " << hdr->len;
        return 0;
    }

    if (!hash_equal(hashhead, mHashKey, data, data_len)) {
        LOGE << "hash not match";
        return 0;
    }

    struct sockaddr_in peer;
    peer.sin_family = AF_INET;
    peer.sin_port = src_port;
    peer.sin_addr.s_addr = ip->ip_src.s_addr;

    struct sockaddr_in self;
    self.sin_family = AF_INET;
    self.sin_port = dst_port;
    self.sin_addr.s_addr = ip->ip_dst.s_addr;

    return cap2uv(ohead, oheadLen, &self, &peer, data, data_len, dst_port, cmd, ackForPeer);
}

void IRawConn::Close() {
    IConn::Close();

    if (mUnixDgramPoll) {
        uv_poll_stop(mUnixDgramPoll);
        close(mWriteFd);
        close(mReadFd);
        free(mUnixDgramPoll);
        mUnixDgramPoll = nullptr;
    }
    if (mTcpNet) {
        libnet_destroy(mTcpNet);
        mTcpNet = nullptr;
    }
    if (mUdpNet) {
        libnet_destroy(mUdpNet);
        mUdpNet = nullptr;
    }
}

void IRawConn::pollCb(uv_poll_t *handle, int status, int events) {
    if (status) {
        LOGE << "unix socket poll err: " << uv_strerror(status);
        return;
    }
    if (events & UV_READABLE) {
        IRawConn *conn = static_cast<IRawConn *>(handle->data);
        char buf[OM_MAX_PKT_SIZE] = {0};
        ssize_t nread = read(conn->mReadFd, buf, OM_MAX_PKT_SIZE);

        if (nread <= OHead::GetEncBufSize()) {
            LOGV << "read broken msg. nread: " << nread << ", minumum required: " << OHead::GetEncBufSize();
#ifndef NNDEBUG
            assert(0);
#else
            return;
#endif
        }

        CMD_TYPE cmd = 0;
        const char *p = decodeCmd(&cmd, buf);
        IUINT32 ackForPeer = 0;
        if (cmd == CMD_TCP) {
            p = decode_uint32(&ackForPeer, p);
        }

        OHead head;
        p = OHead::DecodeBuf(head, p, nread - (p - buf));
        if (!p) {
            LOGV << "failed to decode buf";
#ifndef NNDEBUG
            assert(0);
#else
            return;
#endif
        }

        // todo: add srcInt dstInt
        // todo: how to update omhead field using addr
        struct sockaddr_in peer = {0};
        struct sockaddr_in self = {0};
        p = decode_sockaddr4(p, &self);
        p = decode_sockaddr4(p, &peer);
        IUINT16 dst_port = 0;
        p = decode_uint16(&dst_port, p);    // todo: remove this decode
        head.UpdateDst(self.sin_addr.s_addr);
        head.UpdateSrc(peer.sin_addr.s_addr);
        head.UpdateSourcePort(ntohs(peer.sin_port));
        head.UpdateDstPort(ntohs(self.sin_port));
        head.SetAck(ackForPeer);

        int len = nread - (p - buf);
        if (len > 0) {
            head.srcAddr = reinterpret_cast<sockaddr *>(&peer);
            rbuf_t rbuf = {0};
            rbuf.base = const_cast<char *>(p);
            rbuf.len = len;
            rbuf.data = &head;
            conn->Input(len, rbuf);
        } else {
            LOGE << "len <= 0";
        }
    }
}

int IRawConn::cap2uv(const char *head_beg, size_t head_len, const struct sockaddr_in * self, const struct sockaddr_in *peer, const char *data,
                     size_t data_len, IUINT16 dst_port, CMD_TYPE cmd, IUINT32 ackForPeer) {
    if (data_len + sizeof(struct sockaddr_in) + head_len + sizeof(dst_port) + sizeof(CMD_TYPE) > OM_MAX_PKT_SIZE) {
        LOGE << "drop data_len: " << data_len << ", sizeof(struct sockaddr_in): " << sizeof(struct sockaddr_in)
             << ", head_len: " << head_len;
        return 0;
//        assert(data_len + sizeof(struct sockaddr_in) + head_len + sizeof(dst_port) + sizeof(CMD_TYPE) <=
//               OM_MAX_PKT_SIZE);
    }

    char buf[OM_MAX_PKT_SIZE] = {0};
    char *p = encodeCmd(cmd, buf);
    if (cmd == CMD_TCP) {
        p = encode_uint32(ackForPeer, p);
    }
    memcpy(p, head_beg, head_len);
    p += head_len;
    p = encode_sockaddr4(p, self);
    p = encode_sockaddr4(p, peer);
    p = encode_uint16(dst_port, p);
    memcpy(p, data, data_len);
    p += data_len;
    ssize_t n = write(mWriteFd, buf, p - buf);
    return n;
}

// todo: send ack number too.
int IRawConn::SendRawTcp(libnet_t *l, IUINT32 src, IUINT16 sp, IUINT32 dst, IUINT16 dp, IUINT32 seq, IUINT32 ack,
                         const IUINT8 *payload, IUINT16 payload_len, IUINT16 ip_id, libnet_ptag_t &tcp,
                         libnet_ptag_t &ip, IUINT8 tcp_flag) {
    const int DUMY_WIN_SIZE = 65535;

    tcp = libnet_build_tcp(
            sp,              // source port
            dp,              // dst port
            seq,             // seq
            ack,               // ack number
//            TH_FIN,               // control flag
//            TH_RST,               // control flag
            tcp_flag ,               // control flag
//            0,     // control flag
            DUMY_WIN_SIZE,   // window size
            0,               // check sum. = 0 auto fill
            0,               // urgent pointer
            payload_len + LIBNET_TCP_H,     // total length of the TCP packet (for checksum calculation)
            payload,         // payload
            payload_len,     // playload len
            l,               // pointer to libnet context
            tcp                // protocol tag to modify an existing header, 0 to build a new one
    );

    LOGV << "sp: " << sp << ", dp: " << dp << ", seq: " << seq << ", ack: " << ack << ", payload_len: "
         << payload_len << ", flag: " << tcp_flag;

    if (tcp == -1) {
        LOGE << "failed to build tcp: " << libnet_geterror(l);
        return tcp;
    }
    ip = libnet_build_ipv4(
            payload_len + LIBNET_TCP_H + LIBNET_IPV4_H, /* ip_len total length of the IP packet including all subsequent
                                                 data (subsequent data includes any IP options and IP options padding)*/
            0,              // tos type of service bits
            ip_id,  // id IP identification number
            IP_DF,              // frag fragmentation bits and offset
            TTL_OUT,        // ttl time to live in the network
            IPPROTO_TCP,    // prot upper layer protocol
            0,              // sum checksum (0 for libnet to autofill)
            src,      // src source IPv4 address (little endian)
            dst,// dst destination IPv4 address (little endian)
            NULL,           // payload optional payload or NULL
            0,              // payload_s payload length or 0
            l,              // l pointer to a libnet context
            ip               // ptag protocol tag to modify an existing header, 0 to build a new one

    );

    if (ip == -1) {
        LOGE << "failed to build ipv4: " << libnet_geterror(l);
        return ip;
    }

    int n = libnet_write(l);
    if (-1 == n) {
        LOGE << "libnet_write failed: " << libnet_geterror(l);
        return -1;
    } else {
        in_addr src_in_addr = {src};
        in_addr dst_in_addr = {dst};
        std::string src1 = inet_ntoa(src_in_addr);
        std::string dst1 = inet_ntoa(dst_in_addr);
        LOGV << "libnet_write " << n << " bytes. " << src1 << ":" << sp << "<->" << dst1 << ":" << dp;

    }
    return payload_len;
}

int IRawConn::SendRawUdp(libnet_t *l, IUINT32 src, IUINT16 sp, IUINT32 dst, IUINT16 dp, const IUINT8 *payload,
                         IUINT16 payload_len, IUINT16 ip_id, libnet_ptag_t &udp, libnet_ptag_t &ip) {
    udp = libnet_build_udp(
            sp,         // sp source port
            dp,         // dp destination port
            payload_len + LIBNET_UDP_H, // len total length of the UDP packet
            0,                          // sum checksum (0 for libnet to autofill)
            payload,                    // payload optional payload or NULL
            payload_len,                // payload_s payload length or 0
            l,                          // l pointer to a libnet context
            udp                           //  ptag protocol tag to modify an existing header, 0 to build a new one
    );
    if (-1 == udp) {
        LOGE << "failed to build udp: " << libnet_geterror(l);
        return udp;
    }

    ip = libnet_build_ipv4(
            payload_len + LIBNET_UDP_H + LIBNET_IPV4_H, /* ip_len total length of the IP packet including all subsequent
                                                 data (subsequent data includes any IP options and IP options padding)*/
            0,              // tos type of service bits
            ip_id,  // id IP identification number
            IP_DF,              // frag fragmentation bits and offset
            TTL_OUT,        // ttl time to live in the network
            IPPROTO_UDP,    // prot upper layer protocol
            0,              // sum checksum (0 for libnet to autofill)
            src,  // src source IPv4 address (little endian)
            dst,// dst destination IPv4 address (little endian)
            NULL,           // payload optional payload or NULL
            0,              // payload_s payload length or 0
            l,              // l pointer to a libnet context
            ip               // ptag protocol tag to modify an existing header, 0 to build a new one
    );

    if (-1 == ip) {
        LOGE << "failed to build ipv4: " << libnet_geterror(l);
        return ip;
    }

    int n = libnet_write(l);
    if (-1 == n) {
        LOGE << "libnet_write failed: " << libnet_geterror(l);
        return -1;
    } else {
#ifndef RSOCK_NNDEBUG
        in_addr src_addr = {src};
        in_addr dst_addr = {dst};
        char srcbuf[32] = {0};
        char dstbuf[32] = {0};
        const char *s1 = inet_ntoa(src_addr);
        memcpy(srcbuf, s1, strlen(s1));
        s1 = inet_ntoa(dst_addr);
        memcpy(dstbuf, s1, strlen(s1));
        LOGV << "libnet_write " << n << " bytes " << srcbuf << ":" << sp << "<->" << dstbuf << ":" << dp;
#endif
    }
    return payload_len;
}

const char *IRawConn::decodeCmd(IRawConn::CMD_TYPE *cmd, const char *p) {
    return decode_uint8(cmd, p);
}

char *IRawConn::encodeCmd(IRawConn::CMD_TYPE cmd, char *p) {
    return encode_uint8(cmd, p);
}