/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cppunit/extensions/HelperMacros.h>
#include "CppAssertHelper.h"

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/select.h>

#include "CollectionUtil.h"
#include "ThreadingUtil.h"

using namespace Util;

#include "Vector.h"
using namespace std;

#include <cstring>
#include <list>

#include <zookeeper.h>
#include <errno.h>
#include <recordio.h>
#include "Util.h"

#ifdef SASL
#include <zookeeper_sasl.h>
#endif

struct buff_struct_2 {
    int32_t len;
    int32_t off;
    char *buffer;
};

static int Stat_eq(struct Stat* a, struct Stat* b)
{
    if (a->czxid != b->czxid) return 0;
    if (a->mzxid != b->mzxid) return 0;
    if (a->ctime != b->ctime) return 0;
    if (a->mtime != b->mtime) return 0;
    if (a->version != b->version) return 0;
    if (a->cversion != b->cversion) return 0;
    if (a->aversion != b->aversion) return 0;
    if (a->ephemeralOwner != b->ephemeralOwner) return 0;
    if (a->dataLength != b->dataLength) return 0;
    if (a->numChildren != b->numChildren) return 0;
    if (a->pzxid != b->pzxid) return 0;
    return 1;
}
#ifdef THREADED
    static void yield(zhandle_t *zh, int i)
    {
        sleep(i);
    }
#else
    static void yield(zhandle_t *zh, int seconds)
    {
        int fd;
        int interest;
        int events;
        struct timeval tv;
        int rc;
        time_t expires = time(0) + seconds;
        time_t timeLeft = seconds;
        fd_set rfds, wfds, efds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_ZERO(&efds);

        while(timeLeft >= 0) {
            zookeeper_interest(zh, &fd, &interest, &tv);
            if (fd != -1) {
                if (interest&ZOOKEEPER_READ) {
                    FD_SET(fd, &rfds);
                } else {
                    FD_CLR(fd, &rfds);
                }
                if (interest&ZOOKEEPER_WRITE) {
                    FD_SET(fd, &wfds);
                } else {
                    FD_CLR(fd, &wfds);
                }
            } else {
                fd = 0;
            }
            FD_SET(0, &rfds);
            if (tv.tv_sec > timeLeft) {
                tv.tv_sec = timeLeft;
            }
            rc = select(fd+1, &rfds, &wfds, &efds, &tv);
            timeLeft = expires - time(0);
            events = 0;
            if (FD_ISSET(fd, &rfds)) {
                events |= ZOOKEEPER_READ;
            }
            if (FD_ISSET(fd, &wfds)) {
                events |= ZOOKEEPER_WRITE;
            }
            zookeeper_process(zh, events);
        }
    }
#endif

typedef struct evt {
    string path;
    int type;
} evt_t;

typedef struct watchCtx {
private:
    list<evt_t> events;
    watchCtx(const watchCtx&);
    watchCtx& operator=(const watchCtx&);
public:
    bool connected;
    zhandle_t *zh;
    Mutex mutex;

    watchCtx() {
        connected = false;
        zh = 0;
    }
    ~watchCtx() {
        if (zh) {
            zookeeper_close(zh);
            zh = 0;
        }
    }

    evt_t getEvent() {
        evt_t evt;
        mutex.acquire();
        CPPUNIT_ASSERT( events.size() > 0);
        evt = events.front();
        events.pop_front();
        mutex.release();
        return evt;
    }

    int countEvents() {
        int count;
        mutex.acquire();
        count = events.size();
        mutex.release();
        return count;
    }

    void putEvent(evt_t evt) {
        mutex.acquire();
        events.push_back(evt);
        mutex.release();
    }

    bool waitForConnected(zhandle_t *zh) {
        time_t expires = time(0) + 10;
        while(!connected && time(0) < expires) {
            yield(zh, 1);
        }
        return connected;
    }
    bool waitForDisconnected(zhandle_t *zh) {
        time_t expires = time(0) + 15;
        while(connected && time(0) < expires) {
            yield(zh, 1);
        }
        return !connected;
    }
} watchctx_t;

class Zookeeper_simpleSystem : public CPPUNIT_NS::TestFixture
{
    CPPUNIT_TEST_SUITE(Zookeeper_simpleSystem);
    CPPUNIT_TEST(testAsyncWatcherAutoReset);
    CPPUNIT_TEST(testDeserializeString);
#ifdef THREADED
    CPPUNIT_TEST(testNullData);
#ifdef ZOO_IPV6_ENABLED
    CPPUNIT_TEST(testIPV6);
#endif
    CPPUNIT_TEST(testPath);
    CPPUNIT_TEST(testPathValidation);
    CPPUNIT_TEST(testPing);
    CPPUNIT_TEST(testAcl);
    CPPUNIT_TEST(testChroot);
    CPPUNIT_TEST(testAuth);
    CPPUNIT_TEST(testHangingClient);
    CPPUNIT_TEST(testWatcherAutoResetWithGlobal);
    CPPUNIT_TEST(testWatcherAutoResetWithLocal);
    CPPUNIT_TEST(testGetChildren2);
    CPPUNIT_TEST(testSasl);
#endif
    CPPUNIT_TEST_SUITE_END();

    static void watcher(zhandle_t *, int type, int state, const char *path,void*v){
        watchctx_t *ctx = (watchctx_t*)v;

        if (state == ZOO_CONNECTED_STATE) {
            ctx->connected = true;
        } else {
            ctx->connected = false;
        }
        if (type != ZOO_SESSION_EVENT) {
            evt_t evt;
            evt.path = path;
            evt.type = type;
            ctx->putEvent(evt);
        }
    }

    static const char hostPorts[];

    const char *getHostPorts() {
        return hostPorts;
    }
    
    zhandle_t *createClient(watchctx_t *ctx) {
        return createClient(hostPorts, ctx);
    }

    zhandle_t *createClient(const char *hp, watchctx_t *ctx) {
        zhandle_t *zk = zookeeper_init(hp, watcher, 10000, 0, ctx, 0);
        ctx->zh = zk;
        sleep(1);
        return zk;
    }
    
    zhandle_t *createchClient(watchctx_t *ctx, const char* chroot) {
        zhandle_t *zk = zookeeper_init(chroot, watcher, 10000, 0, ctx, 0);
        ctx->zh = zk;
        sleep(1);
        return zk;
    }
        
    FILE *logfile;
public:

    Zookeeper_simpleSystem() {
      logfile = openlogfile("Zookeeper_simpleSystem");
    }

    ~Zookeeper_simpleSystem() {
      if (logfile) {
        fflush(logfile);
        fclose(logfile);
        logfile = 0;
      }
    }

    void setUp()
    {
        zoo_set_log_stream(logfile);
    }


    void startServer() {
        char cmd[1024];
        sprintf(cmd, "%s start %s", ZKSERVER_CMD, getHostPorts());
        CPPUNIT_ASSERT(system(cmd) == 0);
    }

    void startServerWithOpts(const char *opts) {
        char cmd[1024];
        sprintf(cmd, "%s start %s %s", ZKSERVER_CMD, getHostPorts(), opts);
        CPPUNIT_ASSERT(system(cmd) == 0);
    }

    void stopServer() {
        char cmd[1024];
        sprintf(cmd, "%s stop %s", ZKSERVER_CMD, getHostPorts());
        CPPUNIT_ASSERT(system(cmd) == 0);
    }

    void tearDown()
    {
    }
    
    /** have a callback in the default watcher **/
    static void default_zoo_watcher(zhandle_t *zzh, int type, int state, const char *path, void *context){
        int zrc = 0;
        struct String_vector str_vec = {0, NULL};
        zrc = zoo_wget_children(zzh, "/mytest", default_zoo_watcher, NULL, &str_vec);
    }
    
    /** this checks for a deadlock in calling zookeeper_close and calls from a default watcher that might get triggered just when zookeeper_close() is in progress **/
    void testHangingClient() {
        int zrc = 0;
        char buff[10] = "testall";
        char path[512];
        watchctx_t *ctx;
        struct String_vector str_vec = {0, NULL};
        zhandle_t *zh = zookeeper_init(hostPorts, NULL, 10000, 0, ctx, 0);
        sleep(1);
        zrc = zoo_create(zh, "/mytest", buff, 10, &ZOO_OPEN_ACL_UNSAFE, 0, path, 512);
        zrc = zoo_wget_children(zh, "/mytest", default_zoo_watcher, NULL, &str_vec);
        zrc = zoo_create(zh, "/mytest/test1", buff, 10, &ZOO_OPEN_ACL_UNSAFE, 0, path, 512);
        zrc = zoo_wget_children(zh, "/mytest", default_zoo_watcher, NULL, &str_vec);
        zrc = zoo_delete(zh, "/mytest/test1", -1);
        zookeeper_close(zh);
    }
    

    void testPing()
    {
        watchctx_t ctxIdle;
        watchctx_t ctxWC;
        zhandle_t *zkIdle = createClient(&ctxIdle);
        zhandle_t *zkWatchCreator = createClient(&ctxWC);

        CPPUNIT_ASSERT(zkIdle);
        CPPUNIT_ASSERT(zkWatchCreator);

        char path[80];
        sprintf(path, "/testping");
        int rc = zoo_create(zkWatchCreator, path, "", 0, &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);

        for(int i = 0; i < 30; i++) {
            sprintf(path, "/testping/%i", i);
            rc = zoo_create(zkWatchCreator, path, "", 0, &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0);
            CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
        }

        for(int i = 0; i < 30; i++) {
            sprintf(path, "/testping/%i", i);
            struct Stat stat;
            rc = zoo_exists(zkIdle, path, 1, &stat);
            CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
        }

        for(int i = 0; i < 30; i++) {
            sprintf(path, "/testping/%i", i);
            usleep(500000);
            rc = zoo_delete(zkWatchCreator, path, -1);
            CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
        }
        struct Stat stat;
        CPPUNIT_ASSERT_EQUAL((int)ZNONODE, zoo_exists(zkIdle, "/testping/0", 0, &stat));
    }

    bool waitForEvent(zhandle_t *zh, watchctx_t *ctx, int seconds) {
        time_t expires = time(0) + seconds;
        while(ctx->countEvents() == 0 && time(0) < expires) {
            yield(zh, 1);
        }
        return ctx->countEvents() > 0;
    }

#define COUNT 100

    static zhandle_t *async_zk;
    static volatile int count;
    static const char* hp_chroot;

    static void statCompletion(int rc, const struct Stat *stat, const void *data) {
        int tmp = (int) (long) data;
        CPPUNIT_ASSERT_EQUAL(tmp, rc);
    }

    static void stringCompletion(int rc, const char *value, const void *data) {
        char *path = (char*)data;

        if (rc == ZCONNECTIONLOSS && path) {
            // Try again
            rc = zoo_acreate(async_zk, path, "", 0,  &ZOO_OPEN_ACL_UNSAFE, 0, stringCompletion, 0);
        } else if (rc != ZOK) {
            // fprintf(stderr, "rc = %d with path = %s\n", rc, (path ? path : "null"));
        }
        if (path) {
            free(path);
        }
    }

    static void create_completion_fn(int rc, const char* value, const void *data) {
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        count++;
    }

    static void waitForCreateCompletion(int seconds) {
        time_t expires = time(0) + seconds;
        while(count == 0 && time(0) < expires) {
            sleep(1);
        }
        count--;
    }

    static void watcher_chroot_fn(zhandle_t *zh, int type,
                                    int state, const char *path,void *watcherCtx) {
        // check for path
        char *client_path = (char *) watcherCtx;
        CPPUNIT_ASSERT(strcmp(client_path, path) == 0);
        count ++;
    }

    static void waitForChrootWatch(int seconds) {
        time_t expires = time(0) + seconds;
        while (count == 0 && time(0) < expires) {
            sleep(1);
        }
        count--;
    }

    static void waitForVoidCompletion(int seconds) {
        time_t expires = time(0) + seconds;
        while(count == 0 && time(0) < expires) {
            sleep(1);
        }
        count--;
    }

    static void voidCompletion(int rc, const void *data) {
        int tmp = (int) (long) data;
        CPPUNIT_ASSERT_EQUAL(tmp, rc);
        count++;
    }

    static int saslDigestInitCompletion(int rc, zhandle_t *zh, zoo_sasl_conn_t *conn,
        const char *serverin, int serverinlen) {
        const char *realm = "realm";
        const char *nonce = "nonce";
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        // response should look like
        // realm="zk-sasl-md5",nonce="4n7iytvP7E9GyRVvGQ8pATPPnXJ0GjOB5rmTzk3a",...
        //LOG_DEBUG(("SASL Response: %s", serverin));
        CPPUNIT_ASSERT(strstr(serverin, realm)!=NULL);
        CPPUNIT_ASSERT(strstr(serverin, nonce)!=NULL);
        return rc;
    }

#ifdef SASL
    static int saslSimpleCallback(void *context __attribute__((unused)), int id,
            const char **result, unsigned *len) {
        const char *user = "super";

        /* paranoia check */
        if (!result) {
            return -1;
        }

        switch (id) {
        case SASL_CB_USER:
            *result = user;
            break;
        case SASL_CB_AUTHNAME:
            *result = user;
            break;
        default:
            return -1;
        }
        return 0;
    }

    static int saslPassCallback(sasl_conn_t *conn, void *context __attribute__((unused)),
            int id, sasl_secret_t **psecret) {
        const char *pass = "test";
        unsigned long int len;

        /* paranoia check */
        if (!psecret) {
            return -1;
        }

        switch (id) {
        case SASL_CB_PASS:
            len = strlen(pass);
            *psecret = (sasl_secret_t *) malloc(sizeof(sasl_secret_t) + len);
            (*psecret)->len = len;
            strcpy((char *)(*psecret)->data, (char *) pass);
            break;
        default:
            return -1;
        }
        return 0;
    }
#endif

    static void verifyCreateFails(const char *path, zhandle_t *zk) {
      CPPUNIT_ASSERT_EQUAL((int)ZBADARGUMENTS, zoo_create(zk,
          path, "", 0, &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0));
    }

    static void verifyCreateOk(const char *path, zhandle_t *zk) {
      CPPUNIT_ASSERT_EQUAL((int)ZOK, zoo_create(zk,
          path, "", 0, &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0));
    }

    static void verifyCreateFailsSeq(const char *path, zhandle_t *zk) {
      CPPUNIT_ASSERT_EQUAL((int)ZBADARGUMENTS, zoo_create(zk,
          path, "", 0, &ZOO_OPEN_ACL_UNSAFE, ZOO_SEQUENCE, 0, 0));
    }

    static void verifyCreateOkSeq(const char *path, zhandle_t *zk) {
      CPPUNIT_ASSERT_EQUAL((int)ZOK, zoo_create(zk,
          path, "", 0, &ZOO_OPEN_ACL_UNSAFE, ZOO_SEQUENCE, 0, 0));
    }


    /**
       returns false if the vectors dont match
    **/
    bool compareAcl(struct ACL_vector acl1, struct ACL_vector acl2) {
        if (acl1.count != acl2.count) {
            return false;
        }
        struct ACL *aclval1 = acl1.data;
        struct ACL *aclval2 = acl2.data;
        if (aclval1->perms != aclval2->perms) {
            return false;
        }
        struct Id id1 = aclval1->id;
        struct Id id2 = aclval2->id;
        if (strcmp(id1.scheme, id2.scheme) != 0) {
            return false;
        }
        if (strcmp(id1.id, id2.id) != 0) {
            return false;
        }
        return true;
    }

    void testDeserializeString() {
        char *val_str;
        int rc = 0;
        int val = -1;
        struct iarchive *ia;
        struct buff_struct_2 *b;
        struct oarchive *oa = create_buffer_oarchive();
        oa->serialize_Int(oa, "int", &val);
        b = (struct buff_struct_2 *) oa->priv;
        ia = create_buffer_iarchive(b->buffer, b->len);
        rc = ia->deserialize_String(ia, "string", &val_str);
        CPPUNIT_ASSERT_EQUAL(-EINVAL, rc);
    }
        
    void testAcl() {
        int rc;
        struct ACL_vector aclvec;
        struct Stat stat;
        watchctx_t ctx;
        zhandle_t *zk = createClient(&ctx);
        rc = zoo_create(zk, "/acl", "", 0,
                        &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
        rc = zoo_get_acl(zk, "/acl", &aclvec, &stat  );
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        bool cmp = compareAcl(ZOO_OPEN_ACL_UNSAFE, aclvec);
        CPPUNIT_ASSERT_EQUAL(true, cmp);
        rc = zoo_set_acl(zk, "/acl", -1, &ZOO_READ_ACL_UNSAFE);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        rc = zoo_get_acl(zk, "/acl", &aclvec, &stat);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        cmp = compareAcl(ZOO_READ_ACL_UNSAFE, aclvec);
        CPPUNIT_ASSERT_EQUAL(true, cmp);
    }


    void testAuth() {
        int rc;
        count = 0;
        watchctx_t ctx1, ctx2, ctx3, ctx4, ctx5;
        zhandle_t *zk = createClient(&ctx1);
        struct ACL_vector nodeAcl;
        struct ACL acl_val;
        rc = zoo_add_auth(0, "", 0, 0, voidCompletion, (void*)-1);
        CPPUNIT_ASSERT_EQUAL((int) ZBADARGUMENTS, rc);

        rc = zoo_add_auth(zk, 0, 0, 0, voidCompletion, (void*)-1);
        CPPUNIT_ASSERT_EQUAL((int) ZBADARGUMENTS, rc);

        // auth as pat, create /tauth1, close session
        rc = zoo_add_auth(zk, "digest", "pat:passwd", 10, voidCompletion,
                          (void*)ZOK);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        waitForVoidCompletion(3);
        CPPUNIT_ASSERT(count == 0);

        rc = zoo_create(zk, "/tauth1", "", 0, &ZOO_CREATOR_ALL_ACL, 0, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);

        {
            //create a new client
            zk = createClient(&ctx4);
            rc = zoo_add_auth(zk, "digest", "", 0, voidCompletion, (void*)ZOK);
            CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
            waitForVoidCompletion(3);
            CPPUNIT_ASSERT(count == 0);

            rc = zoo_add_auth(zk, "digest", "", 0, voidCompletion, (void*)ZOK);
            CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
            waitForVoidCompletion(3);
            CPPUNIT_ASSERT(count == 0);
        }

        //create a new client
        zk = createClient(&ctx2);

        rc = zoo_add_auth(zk, "digest", "pat:passwd2", 11, voidCompletion,
                          (void*)ZOK);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        waitForVoidCompletion(3);
        CPPUNIT_ASSERT(count == 0);

        char buf[1024];
        int blen = sizeof(buf);
        struct Stat stat;
        rc = zoo_get(zk, "/tauth1", 0, buf, &blen, &stat);
        CPPUNIT_ASSERT_EQUAL((int)ZNOAUTH, rc);
        // add auth pat w/correct pass verify success
        rc = zoo_add_auth(zk, "digest", "pat:passwd", 10, voidCompletion,
                          (void*)ZOK);

        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        rc = zoo_get(zk, "/tauth1", 0, buf, &blen, &stat);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
        waitForVoidCompletion(3);
        CPPUNIT_ASSERT(count == 0);
        //create a new client
        zk = createClient(&ctx3);
        rc = zoo_add_auth(zk, "digest", "pat:passwd", 10, voidCompletion, (void*) ZOK);
        waitForVoidCompletion(3);
        CPPUNIT_ASSERT(count == 0);
        rc = zoo_add_auth(zk, "ip", "none", 4, voidCompletion, (void*)ZOK);
        //make the server forget the auths
        waitForVoidCompletion(3);
        CPPUNIT_ASSERT(count == 0);

        stopServer();
        CPPUNIT_ASSERT(ctx3.waitForDisconnected(zk));
        startServer();
        CPPUNIT_ASSERT(ctx3.waitForConnected(zk));
        // now try getting the data
        rc = zoo_get(zk, "/tauth1", 0, buf, &blen, &stat);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
        // also check for get
        rc = zoo_get_acl(zk, "/", &nodeAcl, &stat);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
        // check if the acl has all the perms
        CPPUNIT_ASSERT_EQUAL((int)1, (int)nodeAcl.count);
        acl_val = *(nodeAcl.data);
        CPPUNIT_ASSERT_EQUAL((int) acl_val.perms, ZOO_PERM_ALL);
        // verify on root node
        rc = zoo_set_acl(zk, "/", -1, &ZOO_CREATOR_ALL_ACL);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);

        rc = zoo_set_acl(zk, "/", -1, &ZOO_OPEN_ACL_UNSAFE);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);

        //[ZOOKEEPER-1108], test that auth info is sent to server, if client is not
        //connected to server when zoo_add_auth was called.
        zhandle_t *zk_auth = zookeeper_init(hostPorts, NULL, 10000, 0, NULL, 0);
        rc = zoo_add_auth(zk_auth, "digest", "pat:passwd", 10, voidCompletion, (void*)ZOK);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        sleep(2);
        CPPUNIT_ASSERT(count == 1);
        count  = 0;
        CPPUNIT_ASSERT_EQUAL((int) ZOK, zookeeper_close(zk_auth));
        
        // [ZOOKEEPER-800] zoo_add_auth should return ZINVALIDSTATE if
        // the connection is closed. 
        zhandle_t *zk2 = zookeeper_init(hostPorts, NULL, 10000, 0, NULL, 0);
        sleep(1);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, zookeeper_close(zk2));
        CPPUNIT_ASSERT_EQUAL(0, zoo_state(zk2)); // 0 ==> ZOO_CLOSED_STATE
        rc = zoo_add_auth(zk2, "digest", "pat:passwd", 10, voidCompletion, (void*)ZOK);
        CPPUNIT_ASSERT_EQUAL((int) ZINVALIDSTATE, rc);

        struct sockaddr addr;
        socklen_t addr_len = sizeof(addr);
        zk = createClient(&ctx5);
        stopServer();
        CPPUNIT_ASSERT(ctx5.waitForDisconnected(zk));
        CPPUNIT_ASSERT(zookeeper_get_connected_host(zk, &addr, &addr_len) == NULL);
        addr_len = sizeof(addr);
        startServer();
        CPPUNIT_ASSERT(ctx5.waitForConnected(zk));
        CPPUNIT_ASSERT(zookeeper_get_connected_host(zk, &addr, &addr_len) != NULL);
    }

    void testGetChildren2() {
        int rc;
        watchctx_t ctx;
        zhandle_t *zk = createClient(&ctx);

        rc = zoo_create(zk, "/parent", "", 0, &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);

        rc = zoo_create(zk, "/parent/child_a", "", 0, &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);

        rc = zoo_create(zk, "/parent/child_b", "", 0, &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);

        rc = zoo_create(zk, "/parent/child_c", "", 0, &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);

        rc = zoo_create(zk, "/parent/child_d", "", 0, &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);

        struct String_vector strings;
        struct Stat stat_a, stat_b;

        rc = zoo_get_children2(zk, "/parent", 0, &strings, &stat_a);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);

        rc = zoo_exists(zk, "/parent", 0, &stat_b);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);

        CPPUNIT_ASSERT(Stat_eq(&stat_a, &stat_b));
        CPPUNIT_ASSERT(stat_a.numChildren == 4);
    }

    void testIPV6() {
        watchctx_t ctx;
        zhandle_t *zk = createClient("::1:22181", &ctx);
        CPPUNIT_ASSERT(zk);
        int rc = 0;
        rc = zoo_create(zk, "/ipv6", NULL, -1,
                        &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
    }

    void testNullData() {
        watchctx_t ctx;
        zhandle_t *zk = createClient(&ctx);
        CPPUNIT_ASSERT(zk);
        int rc = 0;
        rc = zoo_create(zk, "/mahadev", NULL, -1,
                        &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        char buffer[512];
        struct Stat stat;
        int len = 512;
        rc = zoo_wget(zk, "/mahadev", NULL, NULL, buffer, &len, &stat);
        CPPUNIT_ASSERT_EQUAL( -1, len);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        rc = zoo_set(zk, "/mahadev", NULL, -1, -1);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        rc = zoo_wget(zk, "/mahadev", NULL, NULL, buffer, &len, &stat);
        CPPUNIT_ASSERT_EQUAL( -1, len);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
    }

    void testPath() {
        watchctx_t ctx;
        char pathbuf[20];
        zhandle_t *zk = createClient(&ctx);
        CPPUNIT_ASSERT(zk);
        int rc = 0;

        memset(pathbuf, 'X', 20);
        rc = zoo_create(zk, "/testpathpath0", "", 0,
                        &ZOO_OPEN_ACL_UNSAFE, 0, pathbuf, 0);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        CPPUNIT_ASSERT_EQUAL('X', pathbuf[0]);

        rc = zoo_create(zk, "/testpathpath1", "", 0,
                        &ZOO_OPEN_ACL_UNSAFE, 0, pathbuf, 1);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        CPPUNIT_ASSERT(strlen(pathbuf) == 0);

        rc = zoo_create(zk, "/testpathpath2", "", 0,
                        &ZOO_OPEN_ACL_UNSAFE, 0, pathbuf, 2);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        CPPUNIT_ASSERT(strcmp(pathbuf, "/") == 0);

        rc = zoo_create(zk, "/testpathpath3", "", 0,
                        &ZOO_OPEN_ACL_UNSAFE, 0, pathbuf, 3);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        CPPUNIT_ASSERT(strcmp(pathbuf, "/t") == 0);

        rc = zoo_create(zk, "/testpathpath7", "", 0,
                        &ZOO_OPEN_ACL_UNSAFE, 0, pathbuf, 15);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        CPPUNIT_ASSERT(strcmp(pathbuf, "/testpathpath7") == 0);

        rc = zoo_create(zk, "/testpathpath8", "", 0,
                        &ZOO_OPEN_ACL_UNSAFE, 0, pathbuf, 16);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        CPPUNIT_ASSERT(strcmp(pathbuf, "/testpathpath8") == 0);
    }

    void testPathValidation() {
        watchctx_t ctx;
        zhandle_t *zk = createClient(&ctx);
        CPPUNIT_ASSERT(zk);

        verifyCreateFails(0, zk);
        verifyCreateFails("", zk);
        verifyCreateFails("//", zk);
        verifyCreateFails("///", zk);
        verifyCreateFails("////", zk);
        verifyCreateFails("/.", zk);
        verifyCreateFails("/..", zk);
        verifyCreateFails("/./", zk);
        verifyCreateFails("/../", zk);
        verifyCreateFails("/foo/./", zk);
        verifyCreateFails("/foo/../", zk);
        verifyCreateFails("/foo/.", zk);
        verifyCreateFails("/foo/..", zk);
        verifyCreateFails("/./.", zk);
        verifyCreateFails("/../..", zk);
        verifyCreateFails("/foo/bar/", zk);
        verifyCreateFails("/foo//bar", zk);
        verifyCreateFails("/foo/bar//", zk);

        verifyCreateFails("foo", zk);
        verifyCreateFails("a", zk);

        // verify that trailing fails, except for seq which adds suffix
        verifyCreateOk("/createseq", zk);
        verifyCreateFails("/createseq/", zk);
        verifyCreateOkSeq("/createseq/", zk);
        verifyCreateOkSeq("/createseq/.", zk);
        verifyCreateOkSeq("/createseq/..", zk);
        verifyCreateFailsSeq("/createseq//", zk);
        verifyCreateFailsSeq("/createseq/./", zk);
        verifyCreateFailsSeq("/createseq/../", zk);

        verifyCreateOk("/.foo", zk);
        verifyCreateOk("/.f.", zk);
        verifyCreateOk("/..f", zk);
        verifyCreateOk("/..f..", zk);
        verifyCreateOk("/f.c", zk);
        verifyCreateOk("/f", zk);
        verifyCreateOk("/f/.f", zk);
        verifyCreateOk("/f/f.", zk);
        verifyCreateOk("/f/..f", zk);
        verifyCreateOk("/f/f..", zk);
        verifyCreateOk("/f/.f/f", zk);
        verifyCreateOk("/f/f./f", zk);
    }

    void testChroot() {
        // the c client async callbacks do
        // not callback with the path, so
        // we dont need to test taht for now
        // we should fix that though soon!
        watchctx_t ctx, ctx_ch;
        zhandle_t *zk, *zk_ch;
        char buf[60];
        int rc, len;
        struct Stat stat;
        const char* data = "garbage";
        const char* retStr = "/chroot";
        const char* root= "/";
        zk_ch = createchClient(&ctx_ch, "127.0.0.1:22181/testch1/mahadev");
        CPPUNIT_ASSERT(zk_ch != NULL);
        zk = createClient(&ctx);
        rc = zoo_create(zk, "/testch1", "", 0, &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        rc = zoo_create(zk, "/testch1/mahadev", data, 7, &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        // try an exists with /
        len = 60;
        rc = zoo_get(zk_ch, "/", 0, buf, &len, &stat);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
        //check if the data is the same
        CPPUNIT_ASSERT(strncmp(buf, data, 7) == 0);
        //check for watches
        rc = zoo_wexists(zk_ch, "/chroot", watcher_chroot_fn, (void *) retStr, &stat);
        //now check if we can do create/delete/get/sets/acls/getChildren and others
        //check create
        rc = zoo_create(zk_ch, "/chroot", "", 0, &ZOO_OPEN_ACL_UNSAFE, 0, 0,0);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        waitForChrootWatch(3);
        CPPUNIT_ASSERT(count == 0);
        rc = zoo_create(zk_ch, "/chroot/child", "", 0, &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        rc = zoo_exists(zk, "/testch1/mahadev/chroot/child", 0, &stat);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);

        rc = zoo_delete(zk_ch, "/chroot/child", -1);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        rc = zoo_exists(zk, "/testch1/mahadev/chroot/child", 0, &stat);
        CPPUNIT_ASSERT_EQUAL((int) ZNONODE, rc);
        rc = zoo_wget(zk_ch, "/chroot", watcher_chroot_fn, (char*) retStr,
                      buf, &len, &stat);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        rc = zoo_set(zk_ch, "/chroot",buf, 3, -1);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        waitForChrootWatch(3);
        CPPUNIT_ASSERT(count == 0);
        // check for getchildren
        struct String_vector children;
        rc = zoo_get_children(zk_ch, "/", 0, &children);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
        CPPUNIT_ASSERT_EQUAL((int)1, (int)children.count);
        //check if te child if chroot
        CPPUNIT_ASSERT(strcmp((retStr+1), children.data[0]) == 0);
        // check for get/set acl
        struct ACL_vector acl;
        rc = zoo_get_acl(zk_ch, "/", &acl, &stat);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
        CPPUNIT_ASSERT_EQUAL((int)1, (int)acl.count);
        CPPUNIT_ASSERT_EQUAL((int)ZOO_PERM_ALL, (int)acl.data->perms);
        // set acl
        rc = zoo_set_acl(zk_ch, "/chroot", -1,  &ZOO_READ_ACL_UNSAFE);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        // see if you add children
        rc = zoo_create(zk_ch, "/chroot/child1", "",0, &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int)ZNOAUTH, rc);
        //add wget children test
        rc = zoo_wget_children(zk_ch, "/", watcher_chroot_fn, (char*) root, &children);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);

        //now create a node
        rc = zoo_create(zk_ch, "/child2", "",0, &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        waitForChrootWatch(3);
        CPPUNIT_ASSERT(count == 0);
        //check for one async call just to make sure
        rc = zoo_acreate(zk_ch, "/child3", "", 0, &ZOO_OPEN_ACL_UNSAFE, 0,
                         create_completion_fn, 0);
        waitForCreateCompletion(3);
        CPPUNIT_ASSERT(count == 0);

        //ZOOKEEPER-1027 correctly return path_buffer without prefixed chroot
        const char* path = "/zookeeper1027";
        char path_buffer[1024];
        int path_buffer_len=sizeof(path_buffer);
        rc = zoo_create(zk_ch, path, "", 0, &ZOO_OPEN_ACL_UNSAFE, 0, path_buffer, path_buffer_len);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        CPPUNIT_ASSERT_EQUAL(string(path), string(path_buffer));
    }

    void testAsyncWatcherAutoReset()
    {
        watchctx_t ctx;
        zhandle_t *zk = createClient(&ctx);
        watchctx_t lctx[COUNT];
        int i;
        char path[80];
        int rc;
        evt_t evt;

        async_zk = zk;
        for(i = 0; i < COUNT; i++) {
            sprintf(path, "/awar%d", i);
            rc = zoo_awexists(zk, path, watcher, &lctx[i], statCompletion, (void*)ZNONODE);
            CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
        }

        yield(zk, 0);

        for(i = 0; i < COUNT/2; i++) {
            sprintf(path, "/awar%d", i);
            rc = zoo_acreate(zk, path, "", 0,  &ZOO_OPEN_ACL_UNSAFE, 0, stringCompletion, strdup(path));
            CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
        }

        yield(zk, 3);
        for(i = 0; i < COUNT/2; i++) {
            sprintf(path, "/awar%d", i);
            CPPUNIT_ASSERT_MESSAGE(path, waitForEvent(zk, &lctx[i], 5));
            evt = lctx[i].getEvent();
            CPPUNIT_ASSERT_EQUAL_MESSAGE(evt.path.c_str(), ZOO_CREATED_EVENT, evt.type);
            CPPUNIT_ASSERT_EQUAL(string(path), evt.path);
        }

        for(i = COUNT/2 + 1; i < COUNT*10; i++) {
            sprintf(path, "/awar%d", i);
            rc = zoo_acreate(zk, path, "", 0,  &ZOO_OPEN_ACL_UNSAFE, 0, stringCompletion, strdup(path));
            CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
        }

        yield(zk, 1);
        stopServer();
        CPPUNIT_ASSERT(ctx.waitForDisconnected(zk));
        startServer();
        CPPUNIT_ASSERT(ctx.waitForConnected(zk));
        yield(zk, 3);
        for(i = COUNT/2+1; i < COUNT; i++) {
            sprintf(path, "/awar%d", i);
            CPPUNIT_ASSERT_MESSAGE(path, waitForEvent(zk, &lctx[i], 5));
            evt = lctx[i].getEvent();
            CPPUNIT_ASSERT_EQUAL_MESSAGE(evt.path, ZOO_CREATED_EVENT, evt.type);
            CPPUNIT_ASSERT_EQUAL(string(path), evt.path);
        }
    }

    void testWatcherAutoReset(zhandle_t *zk, watchctx_t *ctxGlobal,
                              watchctx_t *ctxLocal)
    {
        bool isGlobal = (ctxGlobal == ctxLocal);
        int rc;
        struct Stat stat;
        char buf[1024];
        int blen;
        struct String_vector strings;
        const char *testName;

        rc = zoo_create(zk, "/watchtest", "", 0,
                        &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
        rc = zoo_create(zk, "/watchtest/child", "", 0,
                        &ZOO_OPEN_ACL_UNSAFE, ZOO_EPHEMERAL, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);

        if (isGlobal) {
            testName = "GlobalTest";
            rc = zoo_get_children(zk, "/watchtest", 1, &strings);
            deallocate_String_vector(&strings);
            CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
            blen = sizeof(buf);
            rc = zoo_get(zk, "/watchtest/child", 1, buf, &blen, &stat);
            CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
            rc = zoo_exists(zk, "/watchtest/child2", 1, &stat);
            CPPUNIT_ASSERT_EQUAL((int)ZNONODE, rc);
        } else {
            testName = "LocalTest";
            rc = zoo_wget_children(zk, "/watchtest", watcher, ctxLocal,
                                 &strings);
            deallocate_String_vector(&strings);
            CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
            blen = sizeof(buf);
            rc = zoo_wget(zk, "/watchtest/child", watcher, ctxLocal,
                         buf, &blen, &stat);
            CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
            rc = zoo_wexists(zk, "/watchtest/child2", watcher, ctxLocal,
                            &stat);
            CPPUNIT_ASSERT_EQUAL((int)ZNONODE, rc);
        }

        CPPUNIT_ASSERT(ctxLocal->countEvents() == 0);

        stopServer();
        CPPUNIT_ASSERT_MESSAGE(testName, ctxGlobal->waitForDisconnected(zk));
        startServer();
        CPPUNIT_ASSERT_MESSAGE(testName, ctxLocal->waitForConnected(zk));

        CPPUNIT_ASSERT(ctxLocal->countEvents() == 0);

        rc = zoo_set(zk, "/watchtest/child", "1", 1, -1);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
        struct Stat stat1, stat2;
        rc = zoo_set2(zk, "/watchtest/child", "1", 1, -1, &stat1);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
        CPPUNIT_ASSERT(stat1.version >= 0);
        rc = zoo_set2(zk, "/watchtest/child", "1", 1, stat1.version, &stat2);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
        rc = zoo_set(zk, "/watchtest/child", "1", 1, stat2.version);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
        rc = zoo_create(zk, "/watchtest/child2", "", 0,
                        &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);

        CPPUNIT_ASSERT_MESSAGE(testName, waitForEvent(zk, ctxLocal, 5));

        evt_t evt = ctxLocal->getEvent();
        CPPUNIT_ASSERT_EQUAL_MESSAGE(evt.path, ZOO_CHANGED_EVENT, evt.type);
        CPPUNIT_ASSERT_EQUAL(string("/watchtest/child"), evt.path);

        CPPUNIT_ASSERT_MESSAGE(testName, waitForEvent(zk, ctxLocal, 5));
        // The create will trigget the get children and the
        // exists watches
        evt = ctxLocal->getEvent();
        CPPUNIT_ASSERT_EQUAL_MESSAGE(evt.path, ZOO_CREATED_EVENT, evt.type);
        CPPUNIT_ASSERT_EQUAL(string("/watchtest/child2"), evt.path);
        CPPUNIT_ASSERT_MESSAGE(testName, waitForEvent(zk, ctxLocal, 5));
        evt = ctxLocal->getEvent();
        CPPUNIT_ASSERT_EQUAL_MESSAGE(evt.path, ZOO_CHILD_EVENT, evt.type);
        CPPUNIT_ASSERT_EQUAL(string("/watchtest"), evt.path);

        // Make sure Pings are giving us problems
        sleep(5);

        CPPUNIT_ASSERT(ctxLocal->countEvents() == 0);

        stopServer();
        CPPUNIT_ASSERT_MESSAGE(testName, ctxGlobal->waitForDisconnected(zk));
        startServer();
        CPPUNIT_ASSERT_MESSAGE(testName, ctxGlobal->waitForConnected(zk));

        if (isGlobal) {
            testName = "GlobalTest";
            rc = zoo_get_children(zk, "/watchtest", 1, &strings);
            deallocate_String_vector(&strings);
            CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
            blen = sizeof(buf);
            rc = zoo_get(zk, "/watchtest/child", 1, buf, &blen, &stat);
            CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
            rc = zoo_exists(zk, "/watchtest/child2", 1, &stat);
            CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
        } else {
            testName = "LocalTest";
            rc = zoo_wget_children(zk, "/watchtest", watcher, ctxLocal,
                                 &strings);
            deallocate_String_vector(&strings);
            CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
            blen = sizeof(buf);
            rc = zoo_wget(zk, "/watchtest/child", watcher, ctxLocal,
                         buf, &blen, &stat);
            CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
            rc = zoo_wexists(zk, "/watchtest/child2", watcher, ctxLocal,
                            &stat);
            CPPUNIT_ASSERT_EQUAL((int)ZOK, rc);
        }

        zoo_delete(zk, "/watchtest/child2", -1);

        CPPUNIT_ASSERT_MESSAGE(testName, waitForEvent(zk, ctxLocal, 5));

        evt = ctxLocal->getEvent();
        CPPUNIT_ASSERT_EQUAL_MESSAGE(evt.path, ZOO_DELETED_EVENT, evt.type);
        CPPUNIT_ASSERT_EQUAL(string("/watchtest/child2"), evt.path);

        CPPUNIT_ASSERT_MESSAGE(testName, waitForEvent(zk, ctxLocal, 5));
        evt = ctxLocal->getEvent();
        CPPUNIT_ASSERT_EQUAL_MESSAGE(evt.path, ZOO_CHILD_EVENT, evt.type);
        CPPUNIT_ASSERT_EQUAL(string("/watchtest"), evt.path);

        stopServer();
        CPPUNIT_ASSERT_MESSAGE(testName, ctxGlobal->waitForDisconnected(zk));
        startServer();
        CPPUNIT_ASSERT_MESSAGE(testName, ctxLocal->waitForConnected(zk));

        zoo_delete(zk, "/watchtest/child", -1);
        zoo_delete(zk, "/watchtest", -1);

        CPPUNIT_ASSERT_MESSAGE(testName, waitForEvent(zk, ctxLocal, 5));

        evt = ctxLocal->getEvent();
        CPPUNIT_ASSERT_EQUAL_MESSAGE(evt.path, ZOO_DELETED_EVENT, evt.type);
        CPPUNIT_ASSERT_EQUAL(string("/watchtest/child"), evt.path);

        // Make sure nothing is straggling
        sleep(1);
        CPPUNIT_ASSERT(ctxLocal->countEvents() == 0);
    }

    void testWatcherAutoResetWithGlobal()
    {
      {
        watchctx_t ctx;
        zhandle_t *zk = createClient(&ctx);
        int rc = zoo_create(zk, "/testarwg", "", 0, &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        rc = zoo_create(zk, "/testarwg/arwg", "", 0, &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
      }

      {
        watchctx_t ctx;
        zhandle_t *zk = createchClient(&ctx, "127.0.0.1:22181/testarwg/arwg");

        testWatcherAutoReset(zk, &ctx, &ctx);
      }
    }

    void testWatcherAutoResetWithLocal()
    {
      {
        watchctx_t ctx;
        zhandle_t *zk = createClient(&ctx);
        int rc = zoo_create(zk, "/testarwl", "", 0, &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        rc = zoo_create(zk, "/testarwl/arwl", "", 0, &ZOO_OPEN_ACL_UNSAFE, 0, 0, 0);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
      }

      {
        watchctx_t ctx;
        watchctx_t lctx;
        zhandle_t *zk = createchClient(&ctx, "127.0.0.1:22181/testarwl/arwl");
        testWatcherAutoReset(zk, &ctx, &lctx);
      }
    }

    void testSasl() {
        int rc;
        const char *saslopt = "-sasl";
        count = 0;
        watchctx_t ctx1, ctx2, ctx3, ctx4;

        zoo_sasl_conn_t *sasl_conn;
        const char *supportedmechs;
        const char *mech = "DIGEST-MD5";
        const char *service = "zookeeper";
        const char *host = "zk-sasl-md5";
        int supportedmechcount;

        const char *serverin;
        unsigned serverinlen;
        const char *realm = "realm";
        const char *nonce = "nonce";

        stopServer();
        startServerWithOpts(saslopt);

        // zoo_set_log_stream(stdout);
        // zoo_set_debug_level(ZOO_LOG_LEVEL_DEBUG);

        zhandle_t *zk1 = createClient(&ctx1);
        rc = zoo_sasl(zk1, NULL, (const char *) "", 0, &serverin, &serverinlen);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
        // response should look like
        // realm="zk-sasl-md5",nonce="4n7iytvP7E9GyRVvGQ8pATPPnXJ0GjOB5rmTzk3a",...
        //LOG_DEBUG(("SASL Response: %s", serverin));
        CPPUNIT_ASSERT(strstr(serverin, realm)!=NULL);
        CPPUNIT_ASSERT(strstr(serverin, nonce)!=NULL);

        zhandle_t *zk2 = createClient(&ctx2);
        rc = zoo_asasl(zk2, NULL, (const char *) "", 0, saslDigestInitCompletion);
#ifdef SASL

        sasl_callback_t callbacks[] = {
                { SASL_CB_USER, (int (*)())&saslSimpleCallback, NULL },
                { SASL_CB_AUTHNAME, (int (*)())&saslSimpleCallback, NULL },
                { SASL_CB_PASS, (int (*)())&saslPassCallback, NULL },
                { SASL_CB_LIST_END, NULL, NULL } };

        rc = zoo_sasl_init(callbacks);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);

        zhandle_t *zk3 = createClient(&ctx3);

        rc = zoo_sasl_connect(zk3, (char *) service, (char *) host, &sasl_conn,
                &supportedmechs, &supportedmechcount);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);

        rc = zoo_sasl_authenticate(zk3, sasl_conn, mech, supportedmechs);
        CPPUNIT_ASSERT_EQUAL((int) ZOK, rc);
#endif
        stopServer();
        startServer();

    }
};

volatile int Zookeeper_simpleSystem::count;
zhandle_t *Zookeeper_simpleSystem::async_zk;
const char Zookeeper_simpleSystem::hostPorts[] = "127.0.0.1:22181";
CPPUNIT_TEST_SUITE_REGISTRATION(Zookeeper_simpleSystem);

