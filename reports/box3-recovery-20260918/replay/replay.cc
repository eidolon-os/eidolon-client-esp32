#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>
#include <cassert>
#include "nvs_storage.hpp"
class MemoryPartition: public nvs::Partition {
public:
 std::vector<unsigned char> data;
 size_t erases=0;
 MemoryPartition(const char* path) {std::ifstream f(path,std::ios::binary); f.seekg(0x1000); data.resize(0x4000); f.read((char*)data.data(),data.size()); assert(f.good());}
 const char* get_partition_name() override {return "nvs";}
 esp_err_t read_raw(size_t o,void* p,size_t n) override {assert(o+n<=data.size()); memcpy(p,data.data()+o,n);return ESP_OK;}
 esp_err_t read(size_t o,void* p,size_t n) override {return read_raw(o,p,n);}
 esp_err_t write_raw(size_t o,const void* p,size_t n) override {assert(o+n<=data.size()); auto b=(const unsigned char*)p; for(size_t i=0;i<n;i++){assert((data[o+i]&b[i])==b[i]); data[o+i]&=b[i];} return ESP_OK;}
 esp_err_t write(size_t o,const void* p,size_t n) override {return write_raw(o,p,n);}
 esp_err_t erase_range(size_t o,size_t n) override {assert(o+n<=data.size()); memset(data.data()+o,255,n);erases++;return ESP_OK;}
 uint32_t get_address() override {return 0x9000;}
 uint32_t get_size() override {return data.size();}
 bool get_readonly() override {return false;}
};
int main(int argc,char** argv) {
 assert(argc>=2); MemoryPartition p(argv[1]); std::string mode=argc>2?argv[2]:"baseline"; if(mode=="extra-page") p.data.resize(0x5000,255); nvs::Storage s(&p);
 auto r=s.init(0,p.data.size()/4096); std::cout<<"init="<<r<<"\n"; assert(r==0);
 uint8_t ns; assert(s.createOrOpenNamespace("eidolon_id",false,ns)==0);
 size_t size=0; assert(s.getItemDataSize(ns,nvs::ItemType::SZ,"id_pending",size)==0);
 std::vector<char> pending(size); assert(s.readItem(ns,nvs::ItemType::SZ,"id_pending",pending.data(),size)==0);
 std::cout<<"candidate_bytes_with_nul="<<size<<"\n";
 if(mode=="drop-cache") {uint8_t app_ns; assert(s.createOrOpenNamespace("eidolon",false,app_ns)==0); assert(s.eraseItem(app_ns,nvs::ItemType::SZ,"config")==0);}
 if(mode=="slot-marker") {uint8_t value=1; r=s.writeItem(ns,"id_slot",value); std::cout<<"slot_marker_result=0x"<<std::hex<<r<<std::dec<<"\n"; assert(r==0);return 0;}
 for(int i=0;i<(mode=="baseline"?4:1);i++) {auto before=p.erases; r=s.writeItem(ns,nvs::ItemType::SZ,"id_active",pending.data(),size);std::cout<<"attempt="<<i+1<<" result=0x"<<std::hex<<r<<std::dec<<" flash_sector_erases="<<p.erases-before<<"\n"; assert(r==(mode=="baseline"?ESP_ERR_NVS_NOT_ENOUGH_SPACE:ESP_OK));}
}
