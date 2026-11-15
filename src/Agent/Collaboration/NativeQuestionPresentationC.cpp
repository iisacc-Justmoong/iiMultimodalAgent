#include "NativeQuestionPresentationC.h"
#include "NativeQuestionViewModel.h"
#include <iiSDKCore/Core/Text/Utf8.h>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace iiMultimodalAgent::agent::presentation;
namespace {
thread_local char lastError[512]{};
void error(const char* message) noexcept { std::snprintf(lastError,sizeof(lastError),"%s",message); }
bool boolean(int value){if(value!=0&&value!=1)throw std::invalid_argument("invalid question Boolean");return value!=0;}
std::string text(const char* value){
    if(!value)throw std::invalid_argument("missing question text");std::string result(value);
    if(result.size()>131072||!ii::core::valid_utf8(result))throw std::invalid_argument("invalid question UTF-8 or length");
    return result;
}
void copyText(const std::string& value,char* buffer,size_t capacity,size_t* length){
    if(!length)throw std::invalid_argument("missing question text length");*length=value.size();
    if(!buffer&&capacity==0)return;
    if(!buffer||capacity<=value.size())throw std::length_error("question text buffer too small");
    std::memcpy(buffer,value.c_str(),value.size()+1);
}
QuestionRequest request(const iiquestion_request_data& value){
    if(value.question_count>4||(value.question_count&&!value.questions))throw std::invalid_argument("invalid question array");
    QuestionRequest result;result.id=text(value.id);result.questions.reserve(value.question_count);
    for(size_t i=0;i<value.question_count;++i){
        const auto& source=value.questions[i];
        if(source.option_count>4||(source.option_count&&!source.options))throw std::invalid_argument("invalid question options");
        Question question{.text=text(source.text),.header=text(source.header),.multiple=boolean(source.multiple)};
        question.options.reserve(source.option_count);
        for(size_t j=0;j<source.option_count;++j){const auto& option=source.options[j];
            question.options.push_back({text(option.label),text(option.description),text(option.preview)});}
        result.questions.push_back(std::move(question));
    }
    return result;
}
template<class T> decltype(auto) requireReply(T&& reply){
    if(!reply){if(reply.error().cause)std::rethrow_exception(reply.error().cause);throw std::runtime_error("question query rejected");}
    return *std::forward<T>(reply);
}
#define QUESTION_TEXT_PROPERTIES(X) \
    X(IIQUESTION_TITLE,title) X(IIQUESTION_DESCRIPTION,description) X(IIQUESTION_WAITING,waiting) X(IIQUESTION_ERROR,error) \
    X(IIQUESTION_OTHER_PLACEHOLDER,otherPlaceholder) X(IIQUESTION_NOTES_PLACEHOLDER,notesPlaceholder) \
    X(IIQUESTION_DECLINE_TEXT,declineText) X(IIQUESTION_SKIP_TEXT,skipText) X(IIQUESTION_SEND_TEXT,sendText)
#define QUESTION_BOOL_PROPERTIES(X) \
    X(IIQUESTION_OPEN,open) X(IIQUESTION_HAS_WAITING,hasWaiting) X(IIQUESTION_HAS_ERROR,hasError) X(IIQUESTION_MOTION_ENABLED,motionEnabled)
constexpr size_t noIndex=(std::numeric_limits<size_t>::max)();
struct Context final {
    std::thread::id owner=std::this_thread::get_id();
    NativeQuestionViewModel model;
    bool alive=true,exhausted=false;
    unsigned callbackDepth=0;
    uint64_t generation=0;
    iiquestion_observer observer=nullptr;
    iiquestion_action action=nullptr;
    void *observerContext=nullptr,*actionContext=nullptr;
    ii::core::ObservableSubscription structural;
    std::vector<ii::core::ObservableSubscription> globalSubscriptions,rowSubscriptions;
};
struct CallbackScope final {
    explicit CallbackScope(Context& value):context(value){++context.callbackDepth;}
    ~CallbackScope(){--context.callbackDepth;}
    Context& context;
};
void notify(const std::shared_ptr<Context>& context,int property,size_t row=noIndex,size_t option=noIndex){
    if(!context->alive||!context->observer)return;
    CallbackScope scope(*context);auto callback=context->observer;
    callback(context->observerContext,property,row,option,context->generation);
}
template<class T> void watch(const std::shared_ptr<Context>& context,ii::core::Observable<T>& value,
                              int property,std::vector<ii::core::ObservableSubscription>& target,
                              size_t row=noIndex,size_t option=noIndex){
    const std::weak_ptr<Context> weak=context;const auto generation=context->generation;
    target.push_back(value.observe([weak,property,row,option,generation](const T&,std::uint64_t){
        if(auto current=weak.lock();current&&current->alive&&
           (row==noIndex||(!current->exhausted&&current->generation==generation)))notify(current,property,row,option);
    }));
}
void watchRows(const std::shared_ptr<Context>& context){
    context->rowSubscriptions.clear();if(!context->observer||context->exhausted)return;
    auto& target=context->rowSubscriptions;const auto& rows=context->model.rows.get();
    for(size_t i=0;i<rows.size();++i){const auto& row=rows[i];
        watch(context,row->other,IIQUESTION_ROW_OTHER,target,i);watch(context,row->notes,IIQUESTION_ROW_NOTES,target,i);
        watch(context,row->preview,IIQUESTION_ROW_PREVIEW,target,i);
        watch(context,row->otherAccessibility,IIQUESTION_ROW_OTHER_ACCESSIBILITY,target,i);
        watch(context,row->notesAccessibility,IIQUESTION_ROW_NOTES_ACCESSIBILITY,target,i);
        for(size_t j=0;j<row->options.size();++j)watch(context,row->options[j]->checked,IIQUESTION_OPTION_CHECKED,target,i,j);
    }
}
void initialize(const std::shared_ptr<Context>& context){
    const std::weak_ptr<Context> weak=context;
    context->structural=context->model.rows.observe([weak](const auto&,std::uint64_t){
        if(auto current=weak.lock();current&&current->alive){
            if(current->generation==(std::numeric_limits<uint64_t>::max)())current->exhausted=true;
            else ++current->generation;
            watchRows(current);notify(current,IIQUESTION_ROWS);
        }
    });
}
void observe(const std::shared_ptr<Context>& context,iiquestion_observer callback,void* user){
    context->globalSubscriptions.clear();context->rowSubscriptions.clear();
    context->observer=callback;context->observerContext=user;if(!callback)return;
    auto& m=context->model;auto& target=context->globalSubscriptions;
#define WATCH(property,member) watch(context,m.member,property,target);
    QUESTION_TEXT_PROPERTIES(WATCH)
    QUESTION_BOOL_PROPERTIES(WATCH)
#undef WATCH
    watchRows(context);
}
bool send(const std::weak_ptr<Context>& weak,int kind,std::string_view id,const QuestionResponse* response=nullptr){
    auto context=weak.lock();if(!context||!context->alive||!context->action)return false;
    std::map<std::string,iiquestion_response_item> fields;
    if(response){
        for(const auto& [question,answer]:response->answers)fields[question]={question.c_str(),answer.c_str(),nullptr,nullptr};
        for(const auto& [question,annotation]:response->annotations){
            auto [found,inserted]=fields.try_emplace(question,iiquestion_response_item{question.c_str(),nullptr,nullptr,nullptr});
            if(!annotation.preview.empty())found->second.preview=annotation.preview.c_str();
            if(!annotation.notes.empty())found->second.notes=annotation.notes.c_str();
        }
    }
    std::vector<iiquestion_response_item> values;values.reserve(fields.size());
    for(const auto& [key,value]:fields)values.push_back(value);
    const std::string identity(id);CallbackScope scope(*context);auto callback=context->action;
    return callback(context->actionContext,kind,identity.c_str(),values.data(),values.size())==1;
}
void setActions(const std::shared_ptr<Context>& context,iiquestion_action callback,void* user){
    context->action=callback;context->actionContext=user;
    if(!callback){context->model.setActions({},{});return;}
    const std::weak_ptr<Context> weak=context;
    context->model.setActions([weak](const QuestionResponse& response){return send(weak,IIQUESTION_ACTION_SUBMIT,response.id,&response);},
                              [weak](std::string_view id){return send(weak,IIQUESTION_ACTION_DECLINE,id);});
}
std::shared_ptr<QuestionRow> row(Context& context,uint64_t generation,size_t index){
    if(context.exhausted||generation!=context.generation)throw std::logic_error("stale question row generation");
    const auto& rows=context.model.rows.get();if(index>=rows.size())throw std::out_of_range("question row index");
    return rows[index];
}
std::shared_ptr<QuestionOptionRow> option(Context& context,uint64_t generation,size_t rowIndex,size_t index){
    auto current=row(context,generation,rowIndex);
    if(index>=current->options.size())throw std::out_of_range("question option index");return current->options[index];
}
}
struct iiquestion_presentation {std::shared_ptr<Context> impl;};
struct iiquestion_model_lease {std::shared_ptr<Context> impl;};
namespace {
std::shared_ptr<Context> checked(iiquestion_presentation* handle){
    if(!handle||!handle->impl||!handle->impl->alive)throw std::invalid_argument("question model is closed");
    auto context=handle->impl;
    if(context->owner!=std::this_thread::get_id())throw std::logic_error("question model requires its owner thread");
    return context;
}
template<class F>int invoke(iiquestion_presentation* handle,F&& function)noexcept{
    try{auto context=checked(handle);function(context);return 0;}
    catch(const std::exception& e){error(e.what());}catch(...){error("unknown question presentation error");}return -1;
}
}
extern "C" {
const char* iiquestion_last_error(void){return lastError;}
iiquestion_presentation* iiquestion_create(void){
    try{auto context=std::make_shared<Context>();initialize(context);return new iiquestion_presentation{std::move(context)};}
    catch(const std::exception& e){error(e.what());}catch(...){error("cannot create question model");}return nullptr;
}
int iiquestion_destroy(iiquestion_presentation* handle){
    if(!handle)return 0;
    try{auto context=checked(handle);context->alive=false;context->observer=nullptr;context->action=nullptr;delete handle;return 0;}
    catch(const std::exception& e){error(e.what());}catch(...){error("cannot destroy question model");}return -1;
}
int iiquestion_observe(iiquestion_presentation* h,iiquestion_observer callback,void* user){return invoke(h,[&](auto c){observe(c,callback,user);});}
int iiquestion_set_actions(iiquestion_presentation* h,iiquestion_action callback,void* user){return invoke(h,[&](auto c){setActions(c,callback,user);});}
int iiquestion_set_strings(iiquestion_presentation* h,const iiquestion_strings* value){return invoke(h,[&](auto c){
    if(!value)throw std::invalid_argument("missing question strings");
    c->model.setStrings({text(value->title),text(value->description),text(value->waiting),text(value->other),text(value->notes),
        text(value->other_accessibility),text(value->notes_accessibility),text(value->decline),text(value->skip),text(value->send)});
});}
int iiquestion_update(iiquestion_presentation* h,const iiquestion_request_data* values,size_t count,const char* message,int* changed){return invoke(h,[&](auto c){
    if(!changed||count>64||(count&&!values))throw std::invalid_argument("invalid question update");
    QuestionUpdate next;next.error=text(message);next.requests.reserve(count);
    for(size_t i=0;i<count;++i)next.requests.push_back(request(values[i]));*changed=requireReply(c->model.update.send(next));
});}
int iiquestion_change(iiquestion_presentation* h,const char* id,const iiquestion_request_data* value,size_t index,int* changed){return invoke(h,[&](auto c){
    if(!changed)throw std::invalid_argument("missing question change result");
    QuestionDelta delta{.id=text(id),.index=index};if(value)delta.value=request(*value);
    *changed=requireReply(c->model.change.send(delta));
});}
int iiquestion_set_error(iiquestion_presentation* h,const char* value,int* changed){return invoke(h,[&](auto c){
    if(!changed)throw std::invalid_argument("missing question error result");*changed=requireReply(c->model.setError.send({text(value)}));
});}
int iiquestion_read(iiquestion_presentation* h,iiquestion_read_callback callback,void* user){return invoke(h,[&](auto c){
    if(!callback)throw std::invalid_argument("missing question read callback");
    const auto snapshot=requireReply(c->model.read.send({}));
    std::vector<std::vector<std::vector<iiquestion_option_data>>> options(snapshot.requests.size());
    std::vector<std::vector<iiquestion_question_data>> questions(snapshot.requests.size());
    std::vector<iiquestion_request_data> requests;requests.reserve(snapshot.requests.size());
    for(size_t i=0;i<snapshot.requests.size();++i){const auto& source=snapshot.requests[i];
        options[i].resize(source.questions.size());questions[i].reserve(source.questions.size());
        for(size_t j=0;j<source.questions.size();++j){const auto& question=source.questions[j];auto& list=options[i][j];
            list.reserve(question.options.size());for(const auto& item:question.options)
                list.push_back({item.label.c_str(),item.description.c_str(),item.preview.c_str()});
            questions[i].push_back({question.text.c_str(),question.header.c_str(),question.multiple,list.data(),list.size()});
        }
        requests.push_back({source.id.c_str(),questions[i].data(),questions[i].size()});
    }
    CallbackScope scope(*c);callback(user,requests.data(),requests.size(),snapshot.error.c_str());
});}
int iiquestion_rows(iiquestion_presentation* h,uint64_t* generation,size_t* count){return invoke(h,[&](auto c){
    if(!generation||!count||c->exhausted)throw std::invalid_argument("question row generation unavailable");
    *generation=c->generation;*count=c->model.rows.get().size();
});}
int iiquestion_boolean(iiquestion_presentation* h,int property,int* value){return invoke(h,[&](auto c){
    if(!value)throw std::invalid_argument("missing question Boolean result");auto& m=c->model;
    switch(property){
#define GET_BOOL(property,member) case property:*value=m.member.get();break;
    QUESTION_BOOL_PROPERTIES(GET_BOOL)
#undef GET_BOOL
    default:throw std::invalid_argument("unknown question Boolean");}
});}
int iiquestion_set_motion(iiquestion_presentation* h,int value){return invoke(h,[&](auto c){
    c->model.motionEnabled.set(boolean(value));
});}
int iiquestion_text(iiquestion_presentation* h,int property,char* buffer,size_t capacity,size_t* length){return invoke(h,[&](auto c){
    auto& m=c->model;std::string value;
    switch(property){
#define GET_TEXT(property,member) case property:value=m.member.get();break;
    QUESTION_TEXT_PROPERTIES(GET_TEXT)
#undef GET_TEXT
    default:throw std::invalid_argument("unknown question text");}copyText(value,buffer,capacity,length);
});}
int iiquestion_row_boolean(iiquestion_presentation* h,uint64_t generation,size_t index,int property,int* value){return invoke(h,[&](auto c){
    if(!value||property!=IIQUESTION_ROW_MULTIPLE)throw std::invalid_argument("unknown question row Boolean");*value=row(*c,generation,index)->multiple;
});}
int iiquestion_row_text(iiquestion_presentation* h,uint64_t generation,size_t index,int property,char* buffer,size_t capacity,size_t* length){return invoke(h,[&](auto c){
    auto current=row(*c,generation,index);std::string value;
    switch(property){case IIQUESTION_ROW_ID:value=current->id;break;case IIQUESTION_ROW_TEXT:value=current->text;break;
    case IIQUESTION_ROW_HEADER:value=current->header;break;case IIQUESTION_ROW_OTHER:value=current->other.get();break;
    case IIQUESTION_ROW_NOTES:value=current->notes.get();break;case IIQUESTION_ROW_PREVIEW:value=current->preview.get();break;
    case IIQUESTION_ROW_OTHER_ACCESSIBILITY:value=current->otherAccessibility.get();break;
    case IIQUESTION_ROW_NOTES_ACCESSIBILITY:value=current->notesAccessibility.get();break;
    default:throw std::invalid_argument("unknown question row text");}copyText(value,buffer,capacity,length);
});}
int iiquestion_options(iiquestion_presentation* h,uint64_t generation,size_t index,size_t* count){return invoke(h,[&](auto c){
    if(!count)throw std::invalid_argument("missing question option count");*count=row(*c,generation,index)->options.size();
});}
int iiquestion_option_boolean(iiquestion_presentation* h,uint64_t generation,size_t rowIndex,size_t index,int property,int* value){return invoke(h,[&](auto c){
    if(!value||property!=IIQUESTION_OPTION_CHECKED)throw std::invalid_argument("unknown question option Boolean");
    *value=option(*c,generation,rowIndex,index)->checked.get();
});}
int iiquestion_option_text(iiquestion_presentation* h,uint64_t generation,size_t rowIndex,size_t index,int property,char* buffer,size_t capacity,size_t* length){return invoke(h,[&](auto c){
    auto current=option(*c,generation,rowIndex,index);std::string value;
    switch(property){case IIQUESTION_OPTION_ID:value=current->id;break;case IIQUESTION_OPTION_LABEL:value=current->label;break;
    case IIQUESTION_OPTION_DESCRIPTION:value=current->description;break;case IIQUESTION_OPTION_PREVIEW:value=current->preview;break;
    default:throw std::invalid_argument("unknown question option text");}copyText(value,buffer,capacity,length);
});}
int iiquestion_select(iiquestion_presentation* h,uint64_t generation,size_t rowIndex,size_t index,int selected){return invoke(h,[&](auto c){
    const auto value=boolean(selected);option(*c,generation,rowIndex,index)->toggled(value);
});}
int iiquestion_preview(iiquestion_presentation* h,uint64_t generation,size_t rowIndex,size_t index){return invoke(h,[&](auto c){
    option(*c,generation,rowIndex,index)->showPreview();
});}
int iiquestion_edit(iiquestion_presentation* h,uint64_t generation,size_t index,int property,const char* value){return invoke(h,[&](auto c){
    auto current=row(*c,generation,index);auto input=text(value);
    if(property==IIQUESTION_ROW_OTHER)current->editOther(std::move(input));
    else if(property==IIQUESTION_ROW_NOTES)current->editNotes(std::move(input));
    else throw std::invalid_argument("question row text is not writable");
});}
int iiquestion_command(iiquestion_presentation* h,int command,int* accepted){return invoke(h,[&](auto c){
    if(!accepted)throw std::invalid_argument("missing question command result");
    switch(command){case IIQUESTION_SUBMIT:*accepted=requireReply(c->model.submitQuery.send({}));break;
    case IIQUESTION_SKIP:*accepted=requireReply(c->model.skipQuery.send({}));break;
    case IIQUESTION_DECLINE:*accepted=requireReply(c->model.declineQuery.send({}));break;
    default:throw std::invalid_argument("unknown question command");}
});}
int iiquestion_acquire_model(iiquestion_presentation* h,iiquestion_model_lease** output){return invoke(h,[&](auto c){
    if(!output)throw std::invalid_argument("missing question lease result");*output=new iiquestion_model_lease{std::move(c)};
});}
int iiquestion_lease_model(iiquestion_model_lease* lease,void** output){
    try{if(!lease||!lease->impl||!output)throw std::invalid_argument("invalid question lease");
        if(lease->impl->owner!=std::this_thread::get_id())throw std::logic_error("question model requires its owner thread");
        *output=&lease->impl->model;return 0;}
    catch(const std::exception& e){error(e.what());}catch(...){error("cannot access question lease");}return -1;
}
int iiquestion_release_model(iiquestion_model_lease* lease){
    if(!lease)return 0;
    try{if(!lease->impl||lease->impl->owner!=std::this_thread::get_id())throw std::logic_error("question model requires its owner thread");
        if(lease->impl->callbackDepth)throw std::logic_error("release question lease after callbacks return");delete lease;return 0;}
    catch(const std::exception& e){error(e.what());}catch(...){error("cannot release question lease");}return -1;
}
}
