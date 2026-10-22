#include "src/spatial_transport/mat_session_source.h"
#include <fstream>
#include <iostream>
int main(int argc,char** argv) {
    try {
        using namespace std::chrono;using spatial_audio::require;
        require(argc==3,"source.raw NEW fixture.raw");
        require(!std::filesystem::exists(argv[2]),"new fixture required");
        spatial_audio::MatReplaySource source(argv[1],1,1);require(source.start(),"source open");
        spatial_audio::Frame frame;require(source.read(frame)==spatial_audio::Read::frame,"source frame");
        {std::ofstream file(argv[2],std::ios::binary);file.write((char*)frame.bytes.data(),frame.bytes.size());require(bool(file),"fixture write");}
        auto manifest=spatial_transport::inspect_replay(argv[2]);
        std::vector<steady_clock::time_point> arrivals;unsigned ends=0;
        spatial_transport::replay_session(*manifest,[&](const uint8_t* data,size_t) {
            if(data[5]==MAT_WIRE_DATA) {
                arrivals.push_back(steady_clock::now());
                if(arrivals.size()==6)std::this_thread::sleep_for(milliseconds(80));
            }
            else if(data[5]==MAT_WIRE_END)++ends;
        });
        require(arrivals.size()==60 && ends==4,"all fragments and END preserved");
        long long minimum=100000000;
        // One overdue slot immediately after blocked emit is okay; subsequent
        // slots must be paced, not emitted as an overdue 80 ms catch-up burst.
        for(size_t i=7;i<arrivals.size();i++)minimum=std::min(minimum,(long long)duration_cast<microseconds>(arrivals[i]-arrivals[i-1]).count());
        require(minimum>=250,"post-stall catch-up burst");
        std::cout<<"PASS 80ms blocked emit; 60 fragments retained; 4 END; post-stall minimum gap "<<minimum<<" us\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
