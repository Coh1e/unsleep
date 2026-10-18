#include "unsleep/core.hpp"
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
int checks=0;
void check(bool value,const char* description){++checks;if(!value)throw std::runtime_error(description);}
template<class F> void rejects(F&& action,const char* description){bool rejected=false;try{action();}catch(const std::invalid_argument&){rejected=true;}check(rejected,description);}
using namespace unsleep;
Effects advance(Model& model,int count,Inputs input={}){Effects effect;for(int i=0;i<count;++i)effect=model.tick(input);return effect;}
void parser_tests(){
    const auto defaults=parse_options(L"");check(defaults.interval_seconds==59,"Default interval");check(defaults.not_when_locked,"Default lock gating");
    auto options=parse_options(L" \t17 -STARTOFF -USESHIFT -ALLOWSS -STES -KEYPRESS -ONAC -NOTIFY -NOICON -SHOWDLG -ONTASKBAR");
    check(options.interval_seconds==17 && options.start_off && options.use_shift && options.allow_screensaver && options.execution_state && options.full_keypress && options.on_ac && options.notify && options.no_icon && options.show_dialog && options.on_taskbar,"Flags and case folding");
    check(parse_options(L"0 -key:65").interval_seconds==59,"Leading zero leaves default");
    check(parse_options(L"-apptoggleshowdlg -apptoggle -appoff -appon -appexit").remote==Remote::exit,"Remote precedence");
    check(parse_options(L"-apptoggleshowdlg").remote==Remote::toggle_show,"Long remote option before prefix");
    check(parse_options(L"-replaceinactiveafter30").remote==Remote::active_thirty,"Hidden remote active-thirty");
    check(parse_options(L"-oldicons -lighticons -darkicons").icons==Icons::dark,"Icon fixed precedence");
    check(parse_options(L"-key:65foo -key:66").key==65,"First match and decimal prefix");
    check(parse_options(L"-key:nonnumeric").key==0,"Defined nonnumeric conversion");
    check(parse_options(L"-watchwindow:MY EDITOR -notify").watch_window==L"my editor -notify","Window suffix includes remaining command line");
    const auto periods=parse_options(L"-activeperiods:0900-1729,1830-1900").periods;
    check(periods.size()==2 && periods[0].begin==540 && periods[0].end==1049,"Period parser");
    check(periods[0].contains(540) && periods[0].contains(1048) && !periods[0].contains(1049),"Half-open period boundary");
    check(parse_options(L"-activeperiods:2300-0100,2400-2500").periods.empty(),"Invalid and overnight ranges are ignored");
    check(parse_options(L"-activehours:xxxxxxxxxxxxxxxxxxxxxxxx").periods[0].end==1439,"Bitmap last minute");
    check(parse_options(L"-activehours:x.......................").periods[0].end==59,"Bitmap run end");
    check(parse_options(L"-activehours:xxxxxxxxxxxxxxxxxxxxxxxx -activeperiods:0900-1000").periods[0].begin==540,"Periods replace bitmap");
    rejects([]{(void)parse_options(L"999999999999999999");},"Overflow rejected");
    rejects([]{(void)parse_options(L"-exitafter:2147483648");},"Option overflow rejected");
    Model wide_key(parse_options(L"1 -key:65536"));check(wide_key.tick({}).key==0,"Defined 16-bit key narrowing");
    rejects([]{(void)parse_options(L"-activefor:-2");},"Unsafe negative duration rejected");
}
void state_tests(){
    Model defaults(parse_options(L""));check(advance(defaults,58).keep_awake==KeepAwake::none,"No early input");auto effect=defaults.tick({});check(effect.keep_awake==KeepAwake::key && effect.key==0x7e && effect.key_up_only,"59-second F15");
    Model shifted(parse_options(L"1 -useshift -keypress -key:65"));effect=shifted.tick({});check(effect.key==65 && !effect.key_up_only,"Explicit key overrides Shift");
    Model shift_held(parse_options(L"1 -exitafter:1"));Inputs shift{};shift.shift_down=true;effect=advance(shift_held,60,shift);check(effect.exit && effect.keep_awake==KeepAwake::none,"Physical Shift suppresses input without skipping clocks");
    Model locked(parse_options(L"1 -exitafter:1"));locked.set_locked(true);effect=advance(locked,120);check(!effect.exit && locked.exit_remaining()==1 && effect.skipped_tail,"Lock gate skips minute tail");locked.set_locked(false);check(advance(locked,60).exit,"Unlock resumes timer");
    Model stes(parse_options(L"1 -stes -onac -watchwindow:missing"));stes.set_locked(true);Inputs blocked{};blocked.ac_power=false;blocked.matching_window=false;check(stes.tick(blocked).keep_awake==KeepAwake::display,"STES bypasses key-mode gates");
    Model allow(parse_options(L"1 -allowss -stes"));check(allow.tick({}).keep_awake==KeepAwake::system,"allowss precedes stes");
    Model power(parse_options(L"1 -onac"));check(power.tick(blocked).skipped_tail,"AC gate");
    Model window(parse_options(L"1 -watchwindow:missing"));check(window.tick(blocked).skipped_tail,"Window gate");
    Model active_for(parse_options(L"-activefor:1"));advance(active_for,60);check(!active_for.active() && active_for.manual_override(),"Active-for expiry");
    Model inactive_for(parse_options(L"-inactivefor:1"));check(!inactive_for.active(),"Inactive-for starts by toggling");advance(inactive_for,60);check(inactive_for.active(),"Inactive-for expiry");
    Model double_off(parse_options(L"-startoff -inactivefor:1"));check(double_off.active(),"Startup: startoff plus inactivefor double toggle");
    Model zero(parse_options(L"-activefor:0 -exitafter:0"));effect=advance(zero,60);check(zero.active() && !effect.exit && zero.active_remaining()==-1,"Zero countdown does not expire immediately");
    Model manual(parse_options(L"-activefor:5 -exitafter:6 -cpu:10 -watchwindow:x"));manual.toggle();check(manual.active_remaining()==-1 && manual.exit_remaining()==-1 && manual.cpu_threshold()==-1,"Manual toggle clears conditions");
    Model timed(parse_options(L""));timed.timed_inactive(15);check(!timed.active() && timed.inactive_remaining()==15,"Inactive menu duration");timed.timed_active(30);check(timed.active() && timed.active_remaining()==30 && timed.inactive_remaining()==-1,"Active menu resets opposing duration");
    Model schedule(parse_options(L"1 -activeperiods:0900-1000"));schedule.refresh_schedule(539);check(!schedule.active(),"Schedule before start");schedule.refresh_schedule(540);check(schedule.active(),"Schedule start");schedule.toggle();schedule.refresh_schedule(541);check(!schedule.active(),"Manual override blocks schedule");schedule.revert(541);check(schedule.active() && !schedule.can_revert(),"Revert restores schedule control");schedule.refresh_schedule(600);check(!schedule.active(),"Schedule end exclusive");
    Model cpu(parse_options(L"-cpu:50"));Inputs low{};low.cpu_percent=10;advance(cpu,4,low);check(cpu.active(),"CPU warmup four ticks");(void)cpu.tick(low);check(!cpu.active() && cpu.cpu_threshold()==-1,"CPU one-shot on fifth tick");
    Model unknown_cpu(parse_options(L"-cpu:50"));advance(unknown_cpu,10);check(unknown_cpu.active(),"Unavailable CPU is not fabricated as zero");
    Model notify(parse_options(L"-notify"));notify.set_active(false);check(notify.take_notifications()==std::vector<std::wstring>{L"Inactive"},"State notification");
    rejects([&]{timed.timed_active(0);},"Invalid menu duration rejected");
}
void boundary_tests(){
    const std::array<std::wstring,10> payloads{L"",L"-",L"-activeperiods:",L"-activeperiods:0",L"-activeperiods:0000-0000",L"-activehours:",L"-watchwindow:",L"-key:+",L"-cpu:-1",L"2147483647 -activefor:2147483647"};
    for(const auto& payload:payloads){Model model(parse_options(payload));advance(model,100);check(true,"Bounded parser input");}
    std::uint32_t state=0x198;
    constexpr std::wstring_view alphabet=L"0123456789x-:, .\tABC";
    for(int round=0;round<5000;++round){
        std::wstring text=(round%2) ? L"-activeperiods:" : L"-activehours:";
        for(int i=0;i<round%80;++i){state=state*1664525U+1013904223U;text+=alphabet[state%alphabet.size()];}
        try{auto options=parse_options(text);for(const auto& period:options.periods)check(period.begin>=0 && period.end<=1439,"Generated period bounds");}
        catch(const std::invalid_argument&){}
    }
}
}
int main(){try{parser_tests();state_tests();boundary_tests();std::cout<<"PASS: "<<checks<<" checks; 5000 malformed-input cases\n";return 0;}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
