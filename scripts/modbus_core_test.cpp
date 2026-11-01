#include "inverter_data.h"
#include <cassert>
#include <cstdio>

int main()
{
    inverter_data::Snapshot s{};
    assert(!s.get(deye::gridPowerW));
    s.raw[deye::dcC] = 1500; assert(s.get(deye::dcC) == 5000);
    s.raw[deye::acC] = 950; assert(s.get(deye::acC) == -500);
    s.raw[deye::acC] = 65535; assert(!s.get(deye::acC));
    s.raw[deye::batteryPercent] = 101; assert(!s.get(deye::batteryPercent));
    s.raw[deye::pv1VoltageV] = 3800; assert(s.get(deye::pv1VoltageV) == 380000);
    s.raw[deye::pv1PowerW] = 940; assert(s.get(deye::pv1PowerW) == 940000);
    s.raw[deye::pv1PowerW] = 0; assert(s.get(deye::pv1PowerW) == 0);
    s.raw[deye::pv1PowerW] = 65535; assert(!s.get(deye::pv1PowerW));
    s.raw[deye::gridPowerW] = 65535; assert(s.get(deye::gridPowerW) == -1000);
    s.raw[deye::batteryCurrentA] = 64536; assert(s.get(deye::batteryCurrentA) == -10000);
    s.raw[deye::batteryVoltageV] = 5120; assert(s.get(deye::batteryVoltageV) == 51200);
    uint8_t data[12]{};
    deye::request(data, 123, 1, {90, 2});
    assert(deye::be16(data) == 123 && deye::be16(data + 2) == 0 && deye::be16(data + 4) == 6);
    assert(data[7] == 3 && deye::be16(data + 8) == 90 && deye::be16(data + 10) == 2);
    uint8_t reply[] = {0, 123, 0, 0, 0, 7, 1, 3, 4, 0, 1, 0, 2};
    assert(deye::valid_header(reply, 123, 1));
    assert(!deye::valid_header(reply, 124, 1));
    assert(!deye::valid_header(reply, 123, 2));
    assert(deye::valid_response(reply, sizeof(reply), 2));
    assert(!deye::valid_response(reply, sizeof(reply) - 1, 2));
    reply[7] = 0x83; assert(!deye::valid_response(reply, sizeof(reply), 2));
    reply[4] = 255; assert(!deye::valid_header(reply, 123, 1));
    unsigned words = 0;
    for (const auto group : deye::GROUPS) { assert(group.count <= 125); words += group.count; }
    assert(words == 26);
    std::puts("Native Modbus decoder/protocol checks passed");
}
