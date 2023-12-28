// Credit to https://rfwumcu.blogspot.com/2023/01/raspberry-pi-pico-w-c-sdk-lwip-ep-1-ntp.html

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/rtc.h"
#include "time.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "modules/ili9341_tft/ili9341.h"
#include "fonts/font_fixedsys_mono_24.h"
#include "fonts/font48.h"
#include "fonts/font36.h"
#include "fonts/wifi.h"
#include "fonts/wifi_off.h"
#include "fonts/clock.h"
#include "math.h"


#define WIFI_SSID   "your-SSID"
#define WIFI_PASS   "your-WIFI_PASSWORD"
#define NTP_SERVER  "pool.ntp.org"
#define NTP_PORT    (123)
bool showAnalog=true;
/* RFC 1305 
 NTP timestamps are represented as a 64-bit unsigned fixed-
point number, in seconds relative to 0h on 1 January 1900. The integer
part is in the first 32 bits and the fraction part in the last 32 bits.*/
#define NTP_DELTA (2208988800) // seconds between 1 Jan 1900 and 1 Jan 1970
#define NTP_MSG_LEN (48)  // ignore Authenticator (optional)

typedef struct NTP_TIME_T {
    ip_addr_t ntp_ipaddr;
    struct udp_pcb *ntp_pcb;
    bool ntp_server_found;
    absolute_time_t ntp_update_time;
    int tcpip_link_state;
} NTP_TIME;
NTP_TIME net_time;

uint8_t wday[7][10] = {"Sunday", "Monday","Tuesday","Wednesday", "Thursday", "Friday", "Saturday"};
void get_ntp_time();

void set_wifi_status_icon(int status) {
    if (status == CYW43_LINK_UP) {
        ili9341_draw_bitmap(270, 20, &wifi);
        
    } else {
        ili9341_draw_bitmap(270, 20, &wifi_off);
    }
}

int64_t alarm_ntp_update_cb(alarm_id_t alarm_id, void* param) {
    cancel_alarm(alarm_id);
    get_ntp_time();
}

void ntp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    NTP_TIME* ntp_time = (NTP_TIME*)arg;
    uint8_t mode=pbuf_get_at(p,0)& 0x07;  // LI[2], VN[3], MODE[3], mode(0x04): server
    uint8_t stratum = pbuf_get_at(p,1);   // straum 
    uint8_t ts[4]={0};
    uint32_t sec_offset;
    if (port == NTP_PORT && ip_addr_cmp(&net_time.ntp_ipaddr, addr) && p->tot_len == NTP_MSG_LEN && mode == 0x04 && stratum != 0) { 
        pbuf_copy_partial(p, ts, sizeof(ts), 40);
        sec_offset = ((uint32_t)ts[0])<<24 | ((uint32_t)ts[1])<<16 | ((uint32_t)ts[2])<<8 | ((uint32_t)ts[3]);
        uint32_t temp = sec_offset - NTP_DELTA+8*60*60; //UTC+8
        time_t utc_sec_offset = temp;
        struct tm *utc = gmtime(&utc_sec_offset);
        datetime_t rtc_time;

        rtc_time.year=utc->tm_year+1900;
        rtc_time.month= utc->tm_mon+1;
        rtc_time.day = utc->tm_mday;
        rtc_time.hour = utc->tm_hour;
        rtc_time.min = utc->tm_min;
        rtc_time.sec = utc->tm_sec;
        rtc_time.dotw = utc->tm_wday;
        if (!rtc_set_datetime(&rtc_time)) printf("set rtc error\n");

        //printf("got ntp response: %02d/%02d/%04d %02d:%02d:%02d\n", utc->tm_mday, utc->tm_mon + 1, utc->tm_year + 1900,
        //        utc->tm_hour, utc->tm_min, utc->tm_sec);
        ntp_time->ntp_update_time = make_timeout_time_ms(21600000); //6*60*60*1000
        add_alarm_at(ntp_time->ntp_update_time, alarm_ntp_update_cb, arg, false);
        
    }
    pbuf_free(p);
}

void ntp_init_data() {
    net_time.ntp_pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
    net_time.ntp_server_found=false;
    net_time.tcpip_link_state = CYW43_LINK_DOWN;
    if (!net_time.ntp_pcb) {
        printf("alloc udp_new error\n");
        return;
    }
    udp_recv(net_time.ntp_pcb, ntp_recv_cb, &net_time);
}

void get_ntp_time() {
    cyw43_arch_lwip_begin();
    struct pbuf *pb = pbuf_alloc(PBUF_TRANSPORT, NTP_MSG_LEN, PBUF_RAM);
    uint8_t *req = (uint8_t *) pb->payload;
    memset(req, 0, NTP_MSG_LEN);
    req[0] = 0x1b;   // 0x00 011 011 (LI:00, VN:3(version), MODE:3 (client))
    udp_sendto(net_time.ntp_pcb, pb, &net_time.ntp_ipaddr, NTP_PORT);
    pbuf_free(pb);
    cyw43_arch_lwip_end();
}

void dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg) {
    NTP_TIME* ntime = (NTP_TIME*)(arg);
    ntime->ntp_ipaddr=*ipaddr;
    printf("ntp server:%s\n", ipaddr_ntoa(&ntime->ntp_ipaddr));
    ntime->ntp_server_found = true;
}

void digitColck(datetime_t *dt) {
    char buf[100];
    sprintf(buf, "%04d-%02d-%02d", dt->year,dt->month,dt->day);
    ili9341_draw_string_withbg(80,20, buf, 0x0000,0xffff,&font36);
    sprintf(buf, "%02d:%02d:%02d", dt->hour,dt->min,dt->sec);
    ili9341_draw_string_withbg(10,80, buf, 0x001f,0xffff,&font48);
    ili9341_draw_string_withbg(20,190, wday[dt->dotw], 0xf800,0xffff,&font_fixedsys_mono_24);
}

void analogClock(datetime_t *dt) {
    double ang; 
    static int offx_h=0, offy_h=0;
    static int offx_m=0, offy_m=0;
    static int offx_s=0, offy_s=0;
    static int pre_offx_h=0, pre_offy_h=0;
    static int pre_offx_m=0, pre_offy_m=0;
    static int pre_offx_s=0, pre_offy_s=0;
       
    // hour hand
    ang=(((dt->hour)%12)*60+dt->min)*M_PI/360-M_PI/2;
    offx_h = 50*cos(ang);
    offy_h = 50*sin(ang);
    
    // minute hand
    ang=((dt->min)*60+dt->sec)*M_PI/1800-M_PI/2;
    offx_m = 70*cos(ang);
    offy_m = 70*sin(ang);
    
    // second hand
    ang = (dt->sec*M_PI)/30.0-M_PI/2;
    offx_s = 80*cos(ang);
    offy_s = 80*sin(ang);
    //clear clock hands
    ili9341_draw_line_width(159-pre_offx_s/7,119-pre_offy_s/7, 159+pre_offx_s, 119+pre_offy_s, 1,0xffff);
    ili9341_draw_circle(159+pre_offx_s, 119+pre_offy_s,3,0xffff);
    ili9341_draw_line_width(159-pre_offx_m/7,119-pre_offy_m/7, 159+pre_offx_m, 119+pre_offy_m, 1,0xffff);
    ili9341_draw_circle(159+pre_offx_m, 119+pre_offy_m,3,0xffff);
    ili9341_draw_line_width(159-pre_offx_h/7,119-pre_offy_h/7, 159+pre_offx_h, 119+pre_offy_h, 3,0xffff);

    //redraw clock hands
    //hour
    ili9341_draw_line_width(159-offx_h/7,119-offy_h/7, 159+offx_h, 119+offy_h, 3,0x0000);
    //min
    ili9341_draw_line_width(159-offx_m/7,119-offy_m/7, 159+offx_m, 119+offy_m, 1,0x001f);
    ili9341_draw_circle(159+offx_m, 119+offy_m,3,0x001f);
    //sec
    ili9341_draw_line_width(159-offx_s/7,119-offy_s/7, 159+offx_s, 119+offy_s, 1,0xf800);
    ili9341_draw_circle(159+offx_s, 119+offy_s,3,0xf800);

    pre_offx_h = offx_h;pre_offy_h=offy_h;
    pre_offx_m = offx_m;pre_offy_m=offy_m;
    pre_offx_s = offx_s;pre_offy_s=offy_s;
    
    ili9341_draw_fill_circle(159,119,5,0xff00);
  
}

bool repeat_timer_cb(repeating_timer_t *rt) {
    static datetime_t dt;
  
    rtc_get_datetime(&dt);
    

if (showAnalog)
    analogClock(&dt);
else
    digitColck(&dt);

/* 
    if (dt.sec == 0) { 
        ili9341_fill_rect(6,6,309, 229, 0xffff);
        set_wifi_status_icon(net_time.tcpip_link_state);
        showAnalog = !showAnalog;
        if (showAnalog)
            ili9341_draw_bitmap(49,9, &clock_bg);
    }
    */
    
    return true;
}

int main()
{
    stdio_init_all();
    rtc_init();
    cyw43_arch_init();
    ili9341_init();
    ntp_init_data();

    // border
    ili9341_draw_rect(0,0,320, 240, 0x07e0);
    ili9341_draw_rect(1,1,318, 238, 0x07e0);
    ili9341_draw_rect(2,2,316, 236, 0x07e0);
    ili9341_draw_rect(3,3,314, 234, 0x07e0);
    ili9341_draw_rect(4,4,312, 232, 0x0000);
    ili9341_draw_rect(5,5,310, 230, 0x0000);
    //analog clock background bitmap
    ili9341_draw_bitmap(49,9, &clock_bg);

    set_wifi_status_icon(net_time.tcpip_link_state);
   
    cyw43_arch_enable_sta_mode();
/* connect to wifi*/
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASS, CYW43_AUTH_WPA2_AES_PSK, 10000)) {
        printf("Wifi connect timeout!\n");
        return 0;
    }
    int wait_secs = 0;
    while (wait_secs < 10) {
        net_time.tcpip_link_state = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (net_time.tcpip_link_state != CYW43_LINK_UP) {
            wait_secs++;
            if (wait_secs == 10) {
                printf("Can not get ip address\n");
                return 0;
            }
            sleep_ms(1000);
        } else {
            break;
        }

    }
    set_wifi_status_icon(net_time.tcpip_link_state);
   
/* get ntp server ip address  */
    int dns_ret;
    absolute_time_t timeout = make_timeout_time_ms(20000);
    while (!net_time.ntp_server_found && absolute_time_diff_us(get_absolute_time(), timeout) > 0) {
        dns_ret = dns_gethostbyname(NTP_SERVER, &net_time.ntp_ipaddr, dns_cb, &net_time);
        if (dns_ret == ERR_OK) break;
        sleep_ms(1000);
    }
    if(!net_time.ntp_server_found) {
        printf("NTP server not found!\n");
        return 0;
    }
    
    get_ntp_time();

   repeating_timer_t rt;
   add_repeating_timer_ms(-1000, repeat_timer_cb, &net_time, &rt);
   int tcpip_stat;
    while(1) {
        sleep_ms(10000);
        tcpip_stat = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (tcpip_stat != net_time.tcpip_link_state) {
            net_time.tcpip_link_state = tcpip_stat;
            if (net_time.tcpip_link_state == CYW43_LINK_UP) {
                get_ntp_time();
            } 
            set_wifi_status_icon(net_time.tcpip_link_state);
        }
    }
       
    return 0;
}