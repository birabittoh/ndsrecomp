#include "runtime_arm.h"
#include "physical_write.h"
#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
const uint32_t* notified=nullptr;
uint32_t notified_count=0,calls=0;
void require(bool condition,const char* message) {
    if(!condition) {std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}
}
uint32_t store_off=0,store_width=0;
}
// What the runner's handler does for an untracked window: publish the
// touched page cells. The tracked-window test below only counts calls.
extern "C" void runtime_note_code_write_pages(const uint32_t* first,uint32_t count) {
    notified=first;notified_count=count;++calls;
    require(first[0]!=0&&first[count-1]!=0,"generation bumps precede notification and skip zero");
}
extern "C" void runtime_note_code_store(const NdsBusFastWin* w,uint32_t off,uint32_t width) {
    store_off=off;store_width=width;
    if(!w->code_gen) {
        const uint32_t first=off>>12;
        runtime_note_code_write_pages(w->gen+first,((off+width-1u)>>12)-first+1u);
    } else ++calls;
}

int main() {
    std::array<uint8_t,4*4096> bytes{},written{};
    std::array<uint32_t,4> generation{0xffffffffu,5,9,11};
    std::array<uint64_t,4> all_blocks;all_blocks.fill(~0ull);
    NdsBusFastWin backing{bytes.data(),written.data(),generation.data(),uint32_t(bytes.size()-1),
                          all_blocks.data(),nullptr};
    nds_busf_note_write(&backing,4094,4);
    require(calls==1&&notified==generation.data()&&notified_count==2,
            "one crossing write publishes its complete physical range once");
    require(generation[0]==1&&generation[1]==6&&generation[2]==9,"only touched generations change");
    for(unsigned i=0;i<written.size();++i)
        require(written[i]==(i>=4094&&i<4098),"exact per-byte write provenance");
    NdsBusFastWin alias=backing;
    nds_busf_note_write(&alias,4096,2);
    require(calls==2&&notified==generation.data()+1&&notified_count==1&&generation[1]==7,
            "aliases publish the same physical cell");
    require(nds_physical_write_may_touch(generation.data(),2,generation.data()+1),"last page included");
    require(!nds_physical_write_may_touch(generation.data()+1,1,generation.data()),"preceding page excluded");
    require(!nds_physical_write_may_touch(generation.data(),2,generation.data()+2),"following page excluded");
    uint32_t other=0;
    require(!nds_physical_write_may_touch(generation.data(),2,&other),"unrelated backing excluded");
    require(nds_physical_write_may_touch(nullptr,0,&other)&&
            nds_physical_write_may_touch(generation.data(),2,nullptr),"unknown identity stays conservative");
    // Tracked window: only stores touching a watched 64-byte block publish.
    std::array<uint64_t,4> watched{};watched[1]=uint64_t(1)<<2; // bytes 4096+128..+191
    std::array<uint32_t,4> code_gen{1,1,1,1};
    NdsBusFastWin tracked{bytes.data(),written.data(),generation.data(),uint32_t(bytes.size()-1),
                          watched.data(),code_gen.data()};
    const uint32_t before_page=generation[1];
    calls=0;
    nds_busf_note_write(&tracked,4096+124,4);
    require(calls==0&&generation[1]==before_page+1,"store before the watched block: page bump, no publication");
    nds_busf_note_write(&tracked,4096+192,1);
    require(calls==0,"store after the watched block does not publish");
    nds_busf_note_write(&tracked,4096+126,4);
    require(calls==1&&store_off==4096+126&&store_width==4,"store crossing into the watched block publishes");
    nds_busf_note_write(&tracked,4096+191,1);
    require(calls==2,"last byte of the watched block publishes");
    watched[0]=uint64_t(1)<<63;calls=0;
    nds_busf_note_write(&tracked,4094,4);
    require(calls==1,"page-crossing store: first page's last block is checked");
    watched[0]=0;watched[1]=1;calls=0;
    nds_busf_note_write(&tracked,4094,4);
    require(calls==1,"page-crossing store: last page's first block is checked");
    std::puts("PHYSICAL_WRITE passed: C ABI publisher, cross-page range, wrap, exact provenance, aliases and conservative guards");
}
