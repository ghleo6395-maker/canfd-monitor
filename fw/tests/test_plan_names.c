/* The desktop save boundary and board SD boundary must accept the same names. */
#include "sim/sim_api.h"
#include "sim/send_plan.h"
#include "platform/stm32h743_revd/send_plan_json.h"
#include <stdio.h>
#include <string.h>

static unsigned checks,failures;
#define CHECK(x) do { checks++; if(!(x)){ failures++; fprintf(stderr,"names line %d: %s\n",__LINE__,#x); } } while(0)

static void name_case(const char *name,bool valid,const send_plan_t *plan)
{
    int board=sp_json_filename(name);
    int host=sp_request_save(name,plan);
    CHECK((board==SP_OK)==valid);
    CHECK((host==SP_OK)==valid);
    CHECK(board==host);
    sp_io_request_t io;
    if(sp_io_peek(&io)) CHECK(sp_io_complete(io.id,SP_OK,"checked")==SP_OK);
}

int main(void)
{
    sim_reset();sim_key(KEY_POWER,1);
    for(unsigned i=0;i<170;i++)sim_tick(10);
    sim_key(KEY_POWER,0);sp_init();
    send_plan_t plan;CHECK(sp_capture(1,&plan)==SP_OK);
    const char *valid[]={"路试A.canplan.json","NAME.CANPLAN.JSON","CON_1.canplan.json",
        "COM10.canplan.json","内部 空格.canplan.json","测试🙂.canplan.json"};
    const char *bad[]={".canplan.json","../x.canplan.json","CON .canplan.json",
        "LPT9  .canplan.json","x\177.canplan.json","x\302\200.canplan.json",
        "x\302\237.canplan.json","x\342\200\213.canplan.json",
        "x\342\200\217.canplan.json","x\342\200\250.canplan.json",
        "x\342\200\256.canplan.json","x\342\201\246.canplan.json",
        "x\342\201\251.canplan.json","x\357\273\277.canplan.json",
        "x\300\257.canplan.json","x\355\240\200.canplan.json",
        "x\364\220\200\200.canplan.json","x\342.canplan.json"};
    for(unsigned i=0;i<sizeof valid/sizeof *valid;i++)name_case(valid[i],true,&plan);
    for(unsigned i=0;i<sizeof bad/sizeof *bad;i++)name_case(bad[i],false,&plan);
    char limit[49];memset(limit,'a',34);strcpy(limit+34,".canplan.json");
    name_case(limit,true,&plan);
    memset(limit,'a',35);strcpy(limit+35,".canplan.json");name_case(limit,false,&plan);
    /* Catalog refresh must also reject a nonportable name without replacing
     * the previously valid plan or its stable identity. */
    sp_file_t file={0};strcpy(file.name,valid[0]);file.plan=plan;
    CHECK(sp_catalog_replace(&file,1)==SP_OK);
    sp_file_t before;CHECK(sp_catalog_get(0,&before));
    strcpy(file.name,bad[7]);CHECK(sp_catalog_replace(&file,1)==SP_ERR_FORMAT);
    sp_file_t after;CHECK(sp_catalog_get(0,&after));
    CHECK(after.token==before.token&&!strcmp(after.name,before.name));
    printf("Plan names desktop/board: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
