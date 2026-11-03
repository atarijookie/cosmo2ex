// 0x6xxx - what apps check before connecting: carrier, ports, config variables, driver table, STinG memory
#include "stest.h"

static void carrierAndPorts(void)
{
    rec_test(0x6001, "carrier_detect()");
    rec_x("ret", STinG_wf_v(tpl->carrier_detect));      // the carrier_detect() macro of transprt.h is broken

    // port names apps might look for - which exist differs per setup, the log shows what this STinG has
    static const char *names[] = { "CosmosEx", "Internal", "Modem 1", "Modem 2", "Ser.2", "EtherNet", "NoSuchPort", "" };
    char key[24];
    int i;

    rec_test(0x6002, "query_port() of port names");
    for(i=0; i<8; i++) {
        ce_snprintf(key, sizeof(key), "q_%s", names[i][0] ? names[i] : "empty");
        int j;
        for(j=0; key[j]; j++) if(key[j] == ' ') key[j] = '_';
        rec_x(key, query_port((char *) names[i]));
    }

    rec_test(0x6003, "on_port() of not existing port");
    rec_x("ret", on_port("NoSuchPort"));
    rec_x("query", query_port("NoSuchPort"));
}

static void configVars(void)
{
    // variables that mail / news / ftp clients read from STinG
    static const char *vars[] = {
        "USERNAME", "HOSTNAME", "FULLNAME", "EMAIL", "DOMAIN", "SMTP_HOST", "POP_HOST", "POP_USERNAME",
        "POP_PASSWORD", "MAILER", "NAMESERVER", "TIME_ZONE", "TIME_SUMMER", "TIME_SERVER", "DNS_CACHE",
        "ALLOCMEM", "ACTIVATE", "MSS", "RCV_WND", "DEF_TTL", "DEF_RTT", "TCP_PORT", "UDP_PORT", 0
    };

    rec_test(0x6010, "getvstr() of unknown variable");
    char *unknown = getvstr("NO_SUCH_VARIABLE_X");
    rec_xs("value", unknown);
    rec_xs("lower", getvstr("username"));       // are names case sensitive?

    char key[24];
    int i;

    rec_test(0x6011, "getvstr() of common variables");
    for(i=0; vars[i]; i++) {
        char *val = getvstr((char *) vars[i]);

        // the value is the user's config, only 'answers like for an unknown name' is exact
        uint8_t likeUnknown = (val == 0) || (unknown && strcmp(val, unknown) == 0);
        ce_snprintf(key, sizeof(key), "%s", vars[i]);
        rec_x(key, !likeUnknown);
        ce_snprintf(key, sizeof(key), "v_%s", vars[i]);
        rec_fs(key, val);
    }

    rec_test(0x6012, "setvstr() new variable, then getvstr()");
    rec_x("set", setvstr("CETEST_VAR", "abc"));
    rec_xs("get", getvstr("CETEST_VAR"));
    rec_x("set2", setvstr("CETEST_VAR", "defgh"));
    rec_xs("get2", getvstr("CETEST_VAR"));
    rec_x("setempty", setvstr("CETEST_VAR", ""));
    rec_xs("get3", getvstr("CETEST_VAR"));
}

static void driverTable(void)
{
    rec_test(0x6020, "get_dftab()");
    rec_x("transport", (void *) (*stingDrivers->get_dftab)(TRANSPORT_DRIVER) == (void *) tpl);
    rec_xp("unknown", (*stingDrivers->get_dftab)("NO_SUCH_LAYER"));
    rec_x("cfg", stingDrivers->cfg != 0);
}

static void memory(void)
{
    rec_test(0x6030, "KRmalloc / KRrealloc / KRfree / KRgetfree");

    int32 freeBefore = KRgetfree(0);
    rec_f("free", freeBefore);
    rec_f("largest", KRgetfree(1));

    char *p = KRmalloc(1000);
    rec_x("malloc", p != 0);
    if(p) {
        memcpy(p, wBuf, 1000);
        char *q = KRrealloc(p, 3000);
        rec_x("realloc", q != 0);
        if(q) {
            rec_x("kept", memcmp(q, wBuf, 1000) == 0);
            p = q;
        }
        KRfree(p);
    }

    rec_x("freeafter", KRgetfree(0) == freeBefore);    // all back?
    rec_xp("huge", KRmalloc(0x7ffffff0));
    char *z = KRmalloc(0);
    rec_x("zero", z != 0);
    if(z) KRfree(z);                                    // KRfree(NULL) is in t_risky.c
}

void testOnline(void)
{
    carrierAndPorts();
    configVars();
    driverTable();
    memory();
}
