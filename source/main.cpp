#include <stdio.h>

#include "wifi_settings.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/rtc.h"
#include "pico/util/datetime.h"

#include "lwip/dns.h"
#include "lwip/pbuf.h" // pbuf means packet buffer
#include "lwip/udp.h"

#define NTP_SERVER  "pool.ntp.org"
#define NTP_PORT    ( 123 )
#define NTP_MSG_LEN ( 48 )      // Apparently this value ignores the authenticator
#define NTP_DELTA (2208988800)  // Seconds between 1 Jan 1900 and 1 Jan 1970

void dnsCallback( const char *name, const ip_addr_t *ipaddress, void *arg );
void getNtpTime();
void ntpRecievedCallback(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);
int64_t alarmNtpUpdateCallback( alarm_id_t alarm_id, void* param );

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
    udp_recv( m_ntpData.ntpPcb, ntpRecievedCallback, &m_ntpData );


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
        dnsReturnCode = dns_gethostbyname( NTP_SERVER, &m_ntpData.ntpIpAddress, dnsCallback, &m_ntpData );
        if( dnsReturnCode == ERR_OK )
            break;

        ++attempts;
        sleep_ms( 1000 );
    }

    getNtpTime();

    sleep_ms( 100 );

    datetime_t t;
    char datetime_buf[256];
    char* datetime_str = &datetime_buf[0];

    while( 1 )
    {
        rtc_get_datetime( &t );
        datetime_to_str( datetime_str, sizeof(datetime_buf), &t );
        printf( "\r%s     ", datetime_str);
        sleep_ms( 1000 );
    }

    printf( "End\n" );
    for( ;; ) {}
}

void dnsCallback( const char *name, const ip_addr_t *ipaddress, void *arg )
{
    t_ntpData* ntpDataPtr = (t_ntpData*) arg;
    ntpDataPtr->ntpIpAddress = *ipaddress;
    ntpDataPtr->ntpServerFound = true;
    printf( "ntp server:%s\n", ipaddr_ntoa( &ntpDataPtr->ntpIpAddress ) );
}

void getNtpTime()
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

void ntpRecievedCallback(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    t_ntpData* ntpData = (t_ntpData*) arg;
    uint8_t mode = pbuf_get_at( p, 0 ) & 0x07; // LI[2], VN[3], MODE[3], mode(0x04): server
    uint8_t stratum = pbuf_get_at( p, 1 );
    uint8_t ts[4] = {0};
    uint32_t secOffset;
    if( ( port == NTP_PORT ) && ( ip_addr_cmp( &m_ntpData.ntpIpAddress, addr ) )
        && ( p->tot_len == NTP_MSG_LEN ) && ( mode == 0x04 ) && ( stratum != 0 ) )
    {
        pbuf_copy_partial( p, ts, sizeof(ts), 40 );
        secOffset = ( ( (uint32_t) ts[0] ) << 24 ) | ( ( (uint32_t) ts[1] ) << 16 ) 
                    | ( ( (uint32_t) ts[2] ) << 8 ) | ( ( (uint32_t) ts[3] ) );
        time_t utcSecOffset = secOffset - NTP_DELTA + (0*60*60); // Change the 0 to 8 for UTC+8, for example
        struct tm *utc = gmtime( &utcSecOffset );
        datetime_t rtcTime;

        rtcTime.year = utc->tm_year + 1900;
        rtcTime.month= utc->tm_mon + 1;
        rtcTime.day = utc->tm_mday;
        rtcTime.hour = utc->tm_hour;
        rtcTime.min = utc->tm_min;
        rtcTime.sec = utc->tm_sec;
        rtcTime.dotw = utc->tm_wday;

        // Now set the pico's RTC
        if( !rtc_set_datetime( &rtcTime ) )
            printf( "Error setting RTC\n" );

        // You can set an 'alarm callback' to run get_ntp_time at set intervals
        // m_ntpData.ntpUpdateTime = make_timeout_time_ms( 6UL*60UL*60UL*1000UL ); // This is a more realistic update period
        m_ntpData.ntpUpdateTime = make_timeout_time_ms( 1000UL*30UL ); // Update every 30 seconds
        add_alarm_at( m_ntpData.ntpUpdateTime, alarmNtpUpdateCallback, arg, false );
    }

}

int64_t alarmNtpUpdateCallback( alarm_id_t alarm_id, void* param )
{
    printf( "Alarm went off, updating the RTC/NTP thing\n" );
    cancel_alarm( alarm_id );
    getNtpTime();
}
