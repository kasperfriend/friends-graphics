#pragma once
#include <map>
#include <string>
#include <cstdint>
namespace renderer {
struct EnvironmentArea { uint32_t id=0,mapId=0,parentId=0,flags=0; std::string name; };
class EnvironmentDatabase {
public:
    bool Load(const std::wstring& path,std::string& error);
    const EnvironmentArea* Area(uint32_t id) const;
    const std::string* Map(uint32_t id) const;
    const std::map<uint32_t,EnvironmentArea>& Areas() const { return areas; }
    size_t AreaCount() const { return areas.size(); }
    bool Authoritative() const {return authoritative;}
private:
    bool authoritative=false;
    std::map<uint32_t,EnvironmentArea> areas;
    std::map<uint32_t,std::string> maps;
};
}
