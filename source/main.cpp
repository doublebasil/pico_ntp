#include <stdio.h>

#include "wifi_settings.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/rtc.h"

#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#define NTP_SERVER  "pool.ntp.org"
#define NTP_PORT    ( 123 )
#define NTP_MSG_LEN ( 48 ) // Apparently this value ignores the authenticator

void dns_callback( const char *name, const ip_addr_t *ipaddress, void *arg );
void get_ntp_time();
void ntp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);

typedef struct
{
    ip_addr_t ntpIpAddress;
    struct udp_pcb *ntpPcb;
    bool ntpServerFound;
    absolute_time_t ntpUpdateTime;
    int tcpipLinkState;
} t_ntpData;

t_ntpData m_ntpData;

int main( void )
{
    // Initialise the debug output
    stdio_init_all();
    // Wait for the stdio to initialise
    for( int i = 0; i < 10; i++ )
    {
        sleep_ms( 100 );
        printf( "Booting\n" );
    }

    // Initialise the pico rtc
    rtc_init();
    // Initialise the wifi driver
    cyw43_arch_init();
    cyw43_arch_enable_sta_mode();

    // Initialise the ntpData struct, which will be used within a callback
    m_ntpData.ntpPcb = udp_new_ip_type( IPADDR_TYPE_ANY );
    m_ntpData.ntpServerFound = false;
    m_ntpData.tcpipLinkState = CYW43_LINK_DOWN;
    // Setup a ntp callback function which sets the rtc
    udp_recv( m_ntpData.ntpPcb, ntp_recv_cb, &m_ntpData );


    // Connect to wifi
    int connectionResult;
    connectionResult = cyw43_arch_wifi_connect_timeout_ms( WIFI_SSID, WIFI_PASSWORD,
        CYW43_AUTH_WPA2_AES_PSK, 10000 );
    if( connectionResult != 0 )
    {
        printf( "Failed to connect to wifi :(\n" );
        for( ;; ) {}
    }
    else
    {
        printf( "Connected to wifi successfully\n" );
    }

    // Wait for link up, might want to add a timeout thing
    while( cyw43_tcpip_link_status( &cyw43_state, CYW43_ITF_STA ) != CYW43_LINK_UP )
    {
        printf( "Waiting for CYW43_LINK_UP...\n" );
        sleep_ms(1000);
    }
    printf("Got CYW43_LINK_UP\n");

    // Get the ntp server ip address
    int dnsReturnCode;
    const int maxAttempts = 5;
    int attempts = 0;
    while( attempts < maxAttempts )
    {
        dnsReturnCode = dns_gethostbyname( NTP_SERVER, &m_ntpData.ntpIpAddress, dns_callback, &m_ntpData );
        if( dnsReturnCode == ERR_OK )
            break;

        ++attempts;
        sleep_ms( 1000 );
    }

    get_ntp_time();

    while(1) {
        sleep_ms( 5000 );
        m_ntpData.tcpipLinkState = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if( m_ntpData.tcpipLinkState == CYW43_LINK_UP )
        {
            printf( "Got NTP time, not sure what to do with it\n" );
            get_ntp_time();
        }
        else
        {
            printf( "WiFi not currently working\n" );
        }

    }

    printf( "End\n" );
    for( ;; ) {}
}

void dns_callback( const char *name, const ip_addr_t *ipaddress, void *arg )
{
    t_ntpData* ntpDataPtr = (t_ntpData*) (arg);
    ntpDataPtr->ntpIpAddress = *ipaddress;
    ntpDataPtr->ntpServerFound = true;
    printf( "ntp server:%s\n", ipaddr_ntoa( &ntpDataPtr->ntpIpAddress ) );
}

void get_ntp_time()
{
    cyw43_arch_lwip_begin();
    struct pbuf *pb = pbuf_alloc( PBUF_TRANSPORT, NTP_MSG_LEN, PBUF_RAM );
    uint8_t *req = (uint8_t*) pb->payload;
    memset( req, 0, NTP_MSG_LEN );
    req[0] = 0x1b;   // 0x00 011 011 (LI:00, VN:3(version), MODE:3 (client))
    udp_sendto( m_ntpData.ntpPcb, pb, &m_ntpData.ntpIpAddress, NTP_PORT );
    pbuf_free( pb );
    cyw43_arch_lwip_end();
}

void ntp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    printf("ntp_recv_cb!\n");
}
