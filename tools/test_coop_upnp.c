// UPnP parsing, against replies shaped as a MiniUPnPd router sends them (structure from a real one; names made generic):
//   cc -I runtime/hle tools/test_coop_upnp.c runtime/hle/coop_upnp.c -lpthread -o /tmp/t && /tmp/t
#include <stdio.h>
#include <string.h>

#include "coop_upnp.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static const char SSDP[] =
    "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
    "USN: uuid:00000000-0000-0000-0000-000000000000::urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\nEXT:\r\n"
    "SERVER: OpenEmbedded/2.0 UPnP/1.1 MiniUPnPd/2.1.20191006\r\nLOCATION: http://192.168.0.1:56688/rootDesc.xml\r\n"
    "OPT: \"http://schemas.upnp.org/upnp/1/0/\"; ns=01\r\n\r\n";

// A gateway, its WAN device, and that device's connection device: the services nest, and Layer3Forwarding and the
// common interface config come first, each with a control URL of its own.
static const char DESC[] =
    "<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\"><specVersion><major>1</major><minor>1</minor>"
    "</specVersion><device><deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>"
    "<friendlyName>Router</friendlyName><serviceList><service>"
    "<serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType><serviceId>urn:upnp-org:serviceId:L3Forwarding1"
    "</serviceId><SCPDURL>/L3F.xml</SCPDURL><controlURL>/ctl/L3F</controlURL><eventSubURL>/evt/L3F</eventSubURL></service>"
    "</serviceList><deviceList><device><deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType><serviceList><service>"
    "<serviceType>urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1</serviceType><SCPDURL>/WANCfg.xml</SCPDURL>"
    "<controlURL>/ctl/CmnIfCfg</controlURL></service></serviceList><deviceList><device>"
    "<deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType><serviceList><service>"
    "<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
    "<serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId><SCPDURL>/WANIPCn.xml</SCPDURL>"
    "<controlURL>/ctl/IPConn</controlURL><eventSubURL>/evt/IPConn</eventSubURL></service></serviceList></device>"
    "</deviceList></device></deviceList></device></root>";

static const char PPP_ABSOLUTE[] =
    "<root><device><serviceList><service><serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType>"
    "<controlURL>http://10.0.0.1:5000/upnp/control/WANPPPConn1</controlURL></service></serviceList></device></root>";

static const char IP_REPLY[] =
    "HTTP/1.1 200 OK\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nConnection: close\r\nContent-Length: 300\r\n\r\n"
    "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:GetExternalIPAddressResponse "
    "xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\"><NewExternalIPAddress>203.0.113.7</NewExternalIPAddress>"
    "</u:GetExternalIPAddressResponse></s:Body></s:Envelope>\r\n";

static const char FAULT[] =
    "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/xml\r\n\r\n<s:Envelope><s:Body><s:Fault><faultcode>s:Client"
    "</faultcode><detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>718</errorCode>"
    "<errorDescription>ConflictInMappingEntry</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>";

int main(void) {
    char host[64], path[128], type[128], control[128], out[128];
    int port = 0, status = 0;
    const char *body;

    CHECK(upnp_parse_location(SSDP, host, sizeof host, &port, path, sizeof path));
    CHECK(!strcmp(host, "192.168.0.1") && port == 56688 && !strcmp(path, "/rootDesc.xml"));
    CHECK(upnp_parse_location("HTTP/1.1 200 OK\r\nLocation: http://router.lan/desc.xml\r\n\r\n", host, sizeof host, &port, path, sizeof path));
    CHECK(!strcmp(host, "router.lan") && port == 80 && !strcmp(path, "/desc.xml"));
    CHECK(!upnp_parse_location("HTTP/1.1 200 OK\r\nST: x\r\n\r\n", host, sizeof host, &port, path, sizeof path));
    CHECK(!upnp_parse_location("HTTP/1.1 200 OK\r\nLOCATION: https://x/y\r\n\r\n", host, sizeof host, &port, path, sizeof path));

    CHECK(upnp_parse_service(DESC, type, sizeof type, control, sizeof control));
    CHECK(!strcmp(type, "urn:schemas-upnp-org:service:WANIPConnection:1") && !strcmp(control, "/ctl/IPConn"));
    CHECK(upnp_parse_service(PPP_ABSOLUTE, type, sizeof type, control, sizeof control));
    CHECK(!strcmp(type, "urn:schemas-upnp-org:service:WANPPPConnection:1") && !strcmp(control, "/upnp/control/WANPPPConn1"));
    CHECK(!upnp_parse_service("<root><serviceType>urn:other</serviceType><controlURL>/x</controlURL></root>", type, sizeof type,
                              control, sizeof control));

    CHECK(upnp_parse_http(IP_REPLY, &status, &body) && status == 200);
    CHECK(upnp_parse_tag(body, "NewExternalIPAddress", out, sizeof out) && !strcmp(out, "203.0.113.7"));
    CHECK(upnp_parse_http(FAULT, &status, &body) && status == 500);
    CHECK(upnp_parse_tag(body, "errorCode", out, sizeof out) && !strcmp(out, "718"));
    CHECK(upnp_parse_tag(body, "errorDescription", out, sizeof out) && !strcmp(out, "ConflictInMappingEntry"));
    CHECK(!upnp_parse_tag(body, "NewExternalIPAddress", out, sizeof out));
    CHECK(!upnp_parse_http("garbage", &status, &body));

    CHECK(!upnp_private_address("203.0.113.7"));
    CHECK(upnp_private_address("192.168.1.20") && upnp_private_address("10.1.2.3") && upnp_private_address("172.20.0.1"));
    CHECK(upnp_private_address("100.72.1.1"));  // (shared: a provider's NAT)
    CHECK(!upnp_private_address("172.32.0.1") && !upnp_private_address("100.128.0.1"));

    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}
