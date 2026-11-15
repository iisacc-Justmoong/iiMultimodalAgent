#include <iiMultimodalAgent/agent/NativeQuestionPresentationC.h>
#include <stdio.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr,"failed: %s (%s)\n",#value,iiquestion_last_error()); return 1; } } while(0)
static int submitted=0,rows_changed=0,reads=0;
static void observe(void* context,int property,size_t row,size_t option,uint64_t generation){
    (void)context;(void)row;(void)option;(void)generation;if(property==IIQUESTION_ROWS)++rows_changed;
}
static int action(void* context,int kind,const char* id,const iiquestion_response_item* items,size_t count){
    (void)context;
    if(kind==IIQUESTION_ACTION_SUBMIT&&!strcmp(id,"first")&&count==1&&
       !strcmp(items[0].question,"Question")&&!strcmp(items[0].answer,"Native")&&
       items[0].preview&&!strcmp(items[0].preview,"Preview"))++submitted;
    return 1;
}
static void read(void* context,const iiquestion_request_data* requests,size_t count,const char* error){
    (void)context;
    if(count==1&&!strcmp(requests[0].id,"first")&&requests[0].question_count==1&&
       requests[0].questions[0].option_count==2&&!error[0])++reads;
}
int main(void){
    iiquestion_presentation* model=iiquestion_create();CHECK(model);
    CHECK(!iiquestion_observe(model,observe,NULL));CHECK(!iiquestion_set_actions(model,action,NULL));
    const iiquestion_option_data options[]={{"Native","Direct","Preview"},{"Web","Browser","Other"}};
    const iiquestion_question_data questions[]={{"Question","Header",0,options,2}};
    const iiquestion_request_data requests[]={{"first",questions,1}};
    int changed=0,accepted=0;
    CHECK(!iiquestion_update(model,requests,1,"",&changed)&&changed&&rows_changed==1);
    CHECK(!iiquestion_read(model,read,NULL)&&reads==1);
    uint64_t generation=0;size_t count=0;
    CHECK(!iiquestion_rows(model,&generation,&count)&&count==1);
    CHECK(!iiquestion_select(model,generation,0,0,1));
    CHECK(!iiquestion_command(model,IIQUESTION_SUBMIT,&accepted)&&accepted&&submitted==1);
    CHECK(!iiquestion_change(model,"first",NULL,0,&changed)&&changed);
    CHECK(iiquestion_edit(model,generation,0,IIQUESTION_ROW_OTHER,"stale")!=0);
    CHECK(strstr(iiquestion_last_error(),"stale"));
    iiquestion_model_lease* lease=NULL;void* pointer=NULL;
    CHECK(!iiquestion_acquire_model(model,&lease)&&lease);
    CHECK(!iiquestion_destroy(model));
    CHECK(!iiquestion_lease_model(lease,&pointer)&&pointer);
    CHECK(!iiquestion_release_model(lease));
    return 0;
}
