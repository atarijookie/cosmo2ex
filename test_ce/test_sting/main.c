//--------------------------------------------------
// STinG compatibility test.
//
// Records what the STinG API returns in normal and edge cases, against the test server in ../sting_server.
// Run it on real STinG (e.g. STinG + CE_ETH.STX) to get the golden log, then on a STinG clone (fake STinG),
// then compare the logs with ../sting_server/sting_diff.py. See ../sting_server/README.md.
//--------------------------------------------------
#include "stest.h"

TPL     *tpl;
uint32_t SERVER_ADDR;
uint16_t serverPortBase = 10000;
uint32_t MY_ADDR;
uint8_t  slowMachine;

uint8_t *rBuf, *wBuf;

DRV_LIST *stingDrivers;

#define SERVER_ADDR_DEFAULT     0xc0a87b8e      // 192.168.123.142
#define INF_FILE                "STNGTEST.INF"  // remembers the server IP and base port, in the current folder

static int32_t findSting(void)                  // in supervisor - reads the cookie jar
{
    uint32_t *work = *(uint32_t **) 0x5a0L;

    stingDrivers = 0;
    if(!work) {
        return 0;
    }

    for(; *work != 0L; work += 2) {
        if(*work == 'STiK') {
            stingDrivers = (DRV_LIST *) work[1];
            break;
        }
    }

    return 0;
}

static int32_t machineTypeInSuper(void)      // getMachineType() reads the cookie jar pointer at 0x5a0 - supervisor only
{
    return getMachineType();
}

static uint8_t initSting(void)
{
    Supexec(findSting);

    if(!stingDrivers) {
        ce_printf("STinG not found (no STiK cookie).\n");
        return 0;
    }

    if(strncmp(stingDrivers->magic, "STiKmagic", 9) != 0) {
        ce_printf("STinG magic mismatch.\n");
        return 0;
    }

    tpl = (TPL *) (*stingDrivers->get_dftab)(TRANSPORT_DRIVER);
    if(!tpl) {
        ce_printf("STinG transport layer not found.\n");
        return 0;
    }

    return 1;
}

static void ipToStr(uint32_t ip, char *s)
{
    ce_snprintf(s, 16, "%d.%d.%d.%d", (int) (ip >> 24) & 0xff, (int) (ip >> 16) & 0xff, (int) (ip >> 8) & 0xff, (int) ip & 0xff);
}

static uint8_t parseIp(const char *s, uint32_t *ip)
{
    uint32_t val = 0;
    int i;

    for(i=0; i<4; i++) {
        int num = 0, digits = 0;

        while(*s >= '0' && *s <= '9') {
            num = (num * 10) + (*s - '0');
            digits++;
            s++;
        }

        if(digits < 1 || digits > 3 || num > 255) {
            return 0;
        }

        val = (val << 8) | num;

        if(i < 3) {
            if(*s != '.') return 0;
            s++;
        }
    }

    *ip = val;
    return 1;
}

static uint16_t parsePort(const char *s, uint16_t def)
{
    uint32_t val = 0;
    int digits = 0;

    while(*s == ' ') s++;
    while(*s >= '0' && *s <= '9' && digits < 6) {
        val = (val * 10) + (*s - '0');
        digits++;
        s++;
    }

    // the server uses base .. base + 20
    return (digits > 0 && val >= 1 && val <= 65515) ? (uint16_t) val : def;
}

// read a line from keyboard, returns its length (0 = just ENTER)
static int readLine(char *buf, int maxLen)
{
    char in[24];
    memset(in, 0, sizeof(in));
    in[0] = (maxLen < 20) ? maxLen : 20;
    Cconrs(in);
    ce_printf("\n");

    int len = in[1];
    memcpy(buf, in + 2, len);
    buf[len] = 0;
    return len;
}

// server IP and base port - from STNGTEST.INF ("<ip> <port>") of the last run, ENTER keeps them
static void getServerIp(void)
{
    char str[40], in[24];
    uint32_t ip = SERVER_ADDR_DEFAULT;
    uint16_t port = 10000;

    int32_t f = Fopen(INF_FILE, 0);
    if(f >= 0) {
        memset(str, 0, sizeof(str));
        Fread((int16_t) f, sizeof(str) - 1, str);
        Fclose((int16_t) f);

        int i;
        for(i=0; str[i]; i++) {
            if(str[i] == '\r' || str[i] == '\n') str[i] = 0;
        }
        parseIp(str, &ip);

        char *sp = str;
        while(*sp && *sp != ' ') sp++;
        port = parsePort(sp, port);
    }

    ipToStr(ip, str);
    ce_printf("Test server IP (ENTER = %s): ", str);
    if(readLine(in, 17) > 0 && !parseIp(in, &ip)) {
        ce_printf("Bad IP, using %s\n", str);
    }

    ce_printf("Test server base port (ENTER = %d): ", port);
    if(readLine(in, 6) > 0) {
        uint16_t p = parsePort(in, 0);
        if(p) port = p;
        else  ce_printf("Bad port, using %d\n", port);
    }

    SERVER_ADDR    = ip;
    serverPortBase = port;

    ipToStr(ip, str);
    int len = strlen(str);
    ce_snprintf(str + len, sizeof(str) - len, " %d", port);

    f = Fcreate(INF_FILE, 0);
    if(f >= 0) {
        Fwrite((int16_t) f, strlen(str), str);
        Fclose((int16_t) f);
    }
}

// our own IP, as STinG puts it into CNgetinfo() of a connected TCP socket - also checks that the server is reachable.
// Returns E_NORMAL, or the error of TCP_open() / TCP_wait_state().
static int16 findMyAddr(void)
{
    int16 waitRes;
    int16 handle = openTcp(SERVER_ADDR, PORT_TCP_ECHO, 2000, 5, &waitRes);

    MY_ADDR = 0;
    if(handle < 0) {
        return handle;
    }

    if(waitRes == E_NORMAL) {
        CIB *cib = CNgetinfo(handle);
        if(cib) {
            MY_ADDR = cib->address.lhost;
        }
    }
    TCP_close(handle, 0, 0);
    return waitRes;
}

static void showMenu(void)
{
    ce_printf("\n\33p[ A ]\33q all comparison tests (1..7)\n");
    ce_printf("\33p[ 1 ]\33q basic: invalid handles, error texts, limits, resolve\n");
    ce_printf("\33p[ 2 ]\33q data: echo, UDP datagrams, CN* functions\n");
    ce_printf("\33p[ 3 ]\33q open: addressing modes, passive open\n");
    ce_printf("\33p[ 4 ]\33q edge: refused, reset, FIN, half close, stall, handles\n");
    ce_printf("\33p[ 5 ]\33q ICMP: handlers, ping, errors seen by handler\n");
    ce_printf("\33p[ 6 ]\33q online checks: carrier, ports, getvstr, KRmalloc\n");
    ce_printf("\33p[ 7 ]\33q odd calls: 0 byte sends, KRfree(NULL)\n");
    ce_printf("\33p[ X ]\33q known STinG 1.26 crashes: TCP_send -1, fragmented UDP\n");
    ce_printf("\33p[ S ]\33q speed (fuzzy only)\n");
    ce_printf("\33p[ D ]\33q FTP download TEST4M.BIN to C:\\ (port 2122)\n");
    ce_printf("\33p[ Q ]\33q quit\n");
}

int test_sting_main(void)
{
    char tmp[100], ipStr[16];

    ce_printf("\33E\33pSTinG compatibility test\33q\n\n");

    if(!initSting()) {
        ce_printf("Press any key...\n");
        Cnecin();
        return 0;
    }

    rBuf = (uint8_t *) Malloc(BUF_SIZE);
    wBuf = (uint8_t *) Malloc(BUF_SIZE);
    if(!rBuf || !wBuf) {
        ce_printf("Not enough RAM for the buffers.\n");
        if(rBuf) Mfree(rBuf);
        if(wBuf) Mfree(wBuf);
        Cnecin();
        return 0;
    }
    fillWriteBuffer();

    ce_printf("Transport: %s / %s / %s\n\n", tpl->module ? tpl->module : "?", tpl->author ? tpl->author : "?",
              tpl->version ? tpl->version : "?");

    ce_printf("Which STinG is this? Its name goes to the log file name.\n");
    ce_printf("  \33p[ R ]\33q real STinG  -> STNGREAL.LOG\n");
    ce_printf("  \33p[ F ]\33q fake STinG  -> STNGFAKE.LOG\n");
    ce_printf("  \33p[ O ]\33q other       -> STNGTEST.LOG\n");

    const char *logName = 0;
    while(!logName) {
        char key = (char) Cnecin();
        if(key >= 'a' && key <= 'z') key -= 32;
        if(key == 'R') logName = "STNGREAL.LOG";
        if(key == 'F') logName = "STNGFAKE.LOG";
        if(key == 'O') logName = "STNGTEST.LOG";
        if(key == 'Q' || key == 27) break;
    }

    if(logName) {
        getServerIp();

        ce_printf("Machine slower than 8 MHz ST, or emulator? Longer timeouts (Y/N): ");
        char key = (char) Cnecin();
        slowMachine = (key == 'y' || key == 'Y');
        ce_printf("\n");

        while(1) {
            showMenu();
            key = (char) Cnecin();
            if(key >= 'a' && key <= 'z') key -= 32;

            if(key == 'Q' || key == 27) {
                break;
            }

            uint16_t which = 0;
            if(key == 'A')               which = 0x7f;
            if(key >= '1' && key <= '7') which = 1 << (key - '1');
            if(key == 'S')               which = 0x80;
            if(key == 'X')               which = 0x100;
            if(key == 'D')               which = 0x200;
            if(!which) {
                continue;
            }

            ce_printf("\33E");
            if(!rec_init(logName)) {
                continue;
            }

            rec_comment("STinG compatibility test log, format: <test> <#|=|~> <key> <value>");
            ce_snprintf(tmp, sizeof(tmp), "module %s", tpl->module ? tpl->module : "?");
            rec_comment(tmp);
            ce_snprintf(tmp, sizeof(tmp), "author %s", tpl->author ? tpl->author : "?");
            rec_comment(tmp);
            ce_snprintf(tmp, sizeof(tmp), "version %s", tpl->version ? tpl->version : "?");
            rec_comment(tmp);
            ipToStr(SERVER_ADDR, ipStr);
            ce_snprintf(tmp, sizeof(tmp), "server %s port %d", ipStr, serverPortBase);
            rec_comment(tmp);
            ce_snprintf(tmp, sizeof(tmp), "tos %x machine %d", getTOSversion(), (int) Supexec(machineTypeInSuper));
            rec_comment(tmp);

            int16 reach = findMyAddr();
            ipToStr(MY_ADDR, ipStr);
            ce_snprintf(tmp, sizeof(tmp), "local %s", ipStr);
            rec_comment(tmp);

            if(reach != E_NORMAL && (which & 0x17f)) {      // results without network are useless for comparing
                const char *errText = get_err_text(reach);
                if(!errText) errText = "?";
                ce_snprintf(tmp, sizeof(tmp), "server not reachable: %d %s", reach, errText);
                rec_comment(tmp);
                ce_printf("\n\33pCan't connect to the test server: %d (%s)\33q\n", reach, errText);
                ce_printf("Check: CosmosEx port active + IP in STNGPORT.CPX, ROUTE.TAB, sting_server running.\n");
                ce_printf("Run the tests anyway? (Y/N) ");
                char k = (char) Cnecin();
                ce_printf("\n");
                if(k != 'y' && k != 'Y') {
                    rec_comment("aborted - server not reachable");
                    rec_done();
                    continue;
                }
            }

            if(which & 0x01) testBasic();
            if(which & 0x02) testData();
            if(which & 0x04) testOpen();
            if(which & 0x08) testEdge();
            if(which & 0x10) testIcmp();
            if(which & 0x20) testOnline();
            if(which & 0x40) testRisky(0);
            if(which & 0x100) testRisky(1);
            if(which & 0x80) testSpeed();
            if(which & 0x200) testFtp();

            rec_comment("end");
            rec_done();
        }
    }

    Mfree(rBuf);
    Mfree(wBuf);
    return 0;
}
