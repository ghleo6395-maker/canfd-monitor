#include "services/power/input_policy.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
static unsigned checks;
#define C(x) do { ++checks; assert(x); } while(0)
int main(void)
{
    input_policy_t p={0}; input_sample_t s={0};
    C(input_policy_step(&p,&s,0)==INPUT_NONE);
    s.vbus=true; C(input_policy_step(&p,&s,1)==INPUT_USB_100);
    s.configured=true; C(input_policy_step(&p,&s,2)==INPUT_USB_500);
    s.suspended=true; C(input_policy_step(&p,&s,3)==INPUT_SUSPEND);
    s.cc_valid=true;s.cc1_mv=1000;
    C(input_policy_step(&p,&s,100)==INPUT_SUSPEND);
    C(input_policy_step(&p,&s,249)==INPUT_SUSPEND);
    C(input_policy_step(&p,&s,250)==INPUT_TYPEC);
    s.cc1_mv=600;C(input_policy_step(&p,&s,251)==INPUT_SUSPEND);
    s.suspended=false;s.cc1_mv=1000;
    C(input_policy_step(&p,&s,252)==INPUT_USB_500);
    C(input_policy_step(&p,&s,402)==INPUT_TYPEC);
    s.cc2_mv=1000;C(input_policy_step(&p,&s,403)==INPUT_USB_500);
    s.cc1_mv=0;C(input_policy_step(&p,&s,404)==INPUT_USB_500);
    C(input_policy_step(&p,&s,554)==INPUT_TYPEC);
    s.cc_valid=false;C(input_policy_step(&p,&s,555)==INPUT_USB_500);
    s.external=true;C(input_policy_step(&p,&s,556)==INPUT_EXTERNAL);
    s.vbus=false;C(input_policy_step(&p,&s,557)==INPUT_EXTERNAL);
    s.external=false;C(input_policy_step(&p,&s,558)==INPUT_NONE);
    s.vbus=true;s.cc_valid=true;s.cc2_mv=1000;s.configured=false;
    C(input_policy_step(&p,&s,UINT32_MAX-100u)==INPUT_USB_100);
    C(input_policy_step(&p,&s,49u)==INPUT_TYPEC);
    for(unsigned mv=0;mv<=3300;mv+=50){
        p=(input_policy_t){0};s.cc2_mv=mv;
        C(input_policy_step(&p,&s,1000)==INPUT_USB_100);
        C(input_policy_step(&p,&s,1150)==(mv>=800&&mv<=2050?INPUT_TYPEC:INPUT_USB_100));
    }
    C(input_policy_expire(INPUT_TYPEC,true,false,false,30)==INPUT_TYPEC);
    C(input_policy_expire(INPUT_TYPEC,true,false,false,31)==INPUT_USB_100);
    C(input_policy_expire(INPUT_TYPEC,true,false,true,31)==INPUT_SUSPEND);
    C(input_policy_expire(INPUT_EXTERNAL,true,false,false,0)==INPUT_USB_100);
    C(input_policy_expire(INPUT_USB_500,true,false,true,0)==INPUT_SUSPEND);
    for(unsigned m=INPUT_NONE;m<=INPUT_SUSPEND;++m){
        C(input_policy_expire(m,false,false,false,0)==INPUT_NONE);
        C(input_policy_expire(m,false,true,false,10000)==INPUT_EXTERNAL);
        C(input_policy_expire(m,true,false,true,10000)==INPUT_SUSPEND);
    }
    printf("Rev E USB input qualification/fault transitions: %u checks passed\n",checks);
}
