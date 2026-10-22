#include "src/globals.h"
#include "src/stream.h"
#include <iostream>
#include <boost/log/core.hpp>
safe::mail_t mail::man;
int main(int argc,char** argv) {
    if(argc!=5)return 2;
    mail::man=std::make_shared<safe::mail_raw_t>();
    // Match an info-level host: do not turn every fragment/FEC send into a
    // synchronous log write (which distorts timing under concurrent builds).
    boost::log::core::get()->set_filter([](const boost::log::attribute_value_set& values) {
        auto severity=values["Severity"].extract<int>();return severity && severity.get()>=2;
    });
    int result=2;
    if(std::string_view(argv[1])=="--mat-rtp-replay")result=stream::mat_rtp_replay_diagnostic(argc-2,argv+2);
    if(std::string_view(argv[1])=="--mat-session-replay") {
        char flag[]="session";char* args[]={argv[2],argv[3],argv[4],flag};
        result=stream::mat_rtp_replay_diagnostic(4,args);
    }
    if(std::string_view(argv[1])=="--audio-rtp-replay")result=stream::audio_rtp_replay_diagnostic(argc-2,argv+2);
    std::cout.flush();std::cerr.flush();
    // Existing Windows send path starts a process-lifetime address watcher.
    // All sender work is joined above; no audio/device/process capture started.
    ExitProcess(static_cast<UINT>(result));
}
