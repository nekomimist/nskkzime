#include "nskkzime/engine.h"
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace nskkzime;
namespace {
void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void equal(std::u32string_view a,std::u32string_view b) { if(a!=b) throw std::runtime_error("expected "+utf8(b)+", got "+utf8(a)); }
template<class F> void rejects(F action) { bool failed=false; try { action(); } catch(const std::exception&) {failed=true;} check(failed,"invalid rule accepted"); }
std::u32string roman(std::string_view text,std::shared_ptr<const RomajiRules> table=RomajiRules::defaults()) {
    Romaji r(table); std::u32string output; for(auto c:text) output+=r.push(c); return output+r.flush();
}
void type(Engine& e,std::string_view text,std::u32string& committed) { for(auto c:text) committed+=e.handle({KeyCode::Character,c}).committed; }
}
int main() {
    const std::vector<std::pair<const char*,std::function<void()>>> cases{
        {"romaji/foreign-sounds",[] { equal(roman("shejechethidhitsatsitsetsoxwaxkaxkexn"),U"しぇじぇちぇてぃでぃつぁつぃつぇつぉゎゕゖん"); }},
        {"romaji/symbols",[] { equal(roman("z z*z,z-z.z/z0z@z[z]z{z}z(z)zhzjzkzl"),U"　※‥〜…・○◎『』〖〗（）←↓↑→"); }},
        {"romaji/standard-n-unchanged",[] { equal(roman("hannou"),U"はんのう"); equal(roman("nn"),U"ん"); equal(roman("konnichiha"),U"こんにちは"); equal(roman("kan'i"),U"かんい"); }},
        {"romaji/custom-override-bom-crlf",[] {
            auto table=RomajiRules::parse("\xef\xbb\xbf# example\r\nversion=1\r\nwi\tゐ\r\n.\t．\r\n,\t，\r\ntp\t東京\r\n");
            check(table->custom_count()==4,"wrong count"); equal(roman("wikatp,.",table),U"ゐか東京，．"); equal(roman("wi"),U"うぃ");
        }},
        {"romaji/ddskk-nn-override",[] { const auto table=RomajiRules::parse("version=1\nnn\tん\n"); equal(roman("hannou",table),U"はんおう"); equal(roman("hannnou",table),U"はんのう"); equal(roman("nn",table),U"ん"); }},
        {"romaji/carry-and-special-rule-priority",[] {
            const auto table=RomajiRules::parse("version=1\njj\tっ\tj\nmm\tむ\n"); equal(roman("ajja",table),U"あっじゃ"); equal(roman("mm",table),U"む");
            Romaji r(table); equal(r.push('j'),U""); equal(r.push('j'),U"っ"); check(r.pending()=="j"&&r.backspace(),"carry lost"); equal(r.flush(),U"");
        }},
        {"romaji/new-longer-n-and-double-prefix",[] { auto table=RomajiRules::parse("version=1\nnna\tなな\nppa\tぱぱ\n"); equal(roman("nnappa",table),U"ななぱぱ"); }},
        {"romaji/failed-prefix-drops-whole-custom-prefix",[] {
            const auto table=RomajiRules::parse("version=1\ntkl\t東京\n");
            equal(roman("tka",table),U"あ"); equal(roman("tk!",table),U"!"); equal(roman("tkl",table),U"東京");
        }},
        {"engine/explicit-continuations-before-mode-commands",[] {
            const auto table=RomajiRules::parse("version=1\ndl\tでる\ndq\tどく\nd/\tど\nnl\tなる\nkk\tくく\n");
            Engine e(demo_dictionary(),false,table); std::u32string output;
            type(e,"dldqd/nlkk",output); equal(output,U"でるどくどなるくく");
            check(e.mode()==Mode::Hiragana&&!e.abbrev(),"explicit rule became a command");
        }},
        {"engine/failed-carry-and-custom-n-before-mode-commands",[] {
            const auto table=RomajiRules::parse("version=1\njj\tっ\tj\nnn\tん\n");
            for(const auto input:{"jjl","nnl"}) {
                Engine e(demo_dictionary(),false,table); std::u32string output; type(e,input,output);
                equal(output,std::string_view(input)=="jjl"?U"っ":U"ん"); check(e.mode()==Mode::Latin&&e.preedit().empty(),"carry blocked Latin");
            }
        }},
        {"romaji/copy-and-clear",[] {
            auto table=RomajiRules::parse("version=1\nwi\tゐ\n"); Romaji r(table); r.push('w'); auto copy=r; equal(copy.push('i'),U"ゐ"); check(r.pending()=="w","copy changed original"); r.clear(); equal(r.push('a'),U"あ");
        }},
        {"romaji/reject-invalid-fields",[] {
            for(const auto text:{"","version=2","version=1\nversion=1","version=1\na\t","version=1\nka\tか\na\tあ\ta",
                "version=1\nk\tく","version=1\nq\tん","version=1\nla\tぁ","version=1\n/a\tあ","version=1\n \t　",
                "version=1\nKA\tか","version=1\nwi\tゐ\nwi\tい","version=1\nwi\tゐ\tbad","version=1\nwi\tゐ\tk\textra",
                "version=1\nwi\t\xff","version=1\nwi\t\x7f","version=1\nwi\t\xef\xbf\xbc"}) rejects([&]{RomajiRules::parse(text);});
            rejects([]{RomajiRules::parse(std::string("version=1\n#\0",12));});
        }},
        {"romaji/limits",[] {
            rejects([]{RomajiRules::parse(std::string(262145,'#'));});
            rejects([]{RomajiRules::parse("version=1\nabcdefghijklmnopq\tあ");});
            rejects([]{RomajiRules::parse("version=1\ntp\t"+std::string(65,'x'));});
            std::string many="version=1\n"; for(int i=0;i<2049;++i) { auto n=std::to_string(i); many+=";"+std::string(4-n.size(),'0')+n+"\tあ\n"; }
            rejects([&]{RomajiRules::parse(many);});
        }},
        {"engine/space-continuation-versus-conversion",[] {
            Engine e(demo_dictionary(),false); std::u32string output; type(e,"z",output); const auto before=e.revision(); check(e.would_handle({KeyCode::Space})&&e.revision()==before,"preflight changed state");
            output+=e.handle({KeyCode::Space}).committed; equal(output,U"　"); check(!e.would_handle({KeyCode::Space}),"idle space eaten");
            type(e,"Kanji",output); e.handle({KeyCode::Space}); equal(e.composition_text(),U"漢字");
            e.reset(); type(e,"Z",output); e.handle({KeyCode::Space}); equal(e.composition_text(),U"　"); check(e.stage()==Stage::Reading,"symbol requested conversion");
        }},
        {"engine/custom-katakana-registration-and-reset",[] {
            auto table=RomajiRules::parse("version=1\nwi\tゐ\n"); Engine e(demo_dictionary(),true,table); std::u32string output;
            type(e,"qwiq",output); equal(output,U"ヰ"); type(e,"Michi",output); e.handle({KeyCode::Space}); check(e.registering(),"registration missing");
            type(e,"wi",output); equal(e.registration_text(),U"ゐ"); output+=e.handle({KeyCode::Enter}).committed; equal(output,U"ヰゐ");
            e.reset(); type(e,"wi",output); equal(output,U"ヰゐゐ");
        }},
        {"engine/custom-shift-okuri",[] {
            auto table=RomajiRules::parse("version=1\nku\tくぅ\n"); Engine e(demo_dictionary(),false,table); std::u32string output;
            type(e,"KAKU",output); equal(e.composition_text(),U"書くぅ");
            e.reset(); type(e,"KYE",output); equal(e.composition_text(),U"きぇ");
        }},
        {"romaji/bounded-random-input",[] {
            auto table=RomajiRules::parse("version=1\njj\tっ\tj\nnn\tん\n;\t🐈\n"); Romaji r(table); std::mt19937 random(381);
            for(int i=0;i<10000;++i) { r.push(static_cast<char>(' '+random()%95)); check(r.pending().size()<=16,"unbounded pending input"); if(i%43==0)r.backspace(); if(i%97==0)r.clear(); }
        }}
    };
    std::size_t passed=0; for(const auto& [name,test]:cases) { try {test(); ++passed; std::cout<<"PASS "<<name<<'\n';} catch(const std::exception& e){std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';} }
    std::cout<<passed<<'/'<<cases.size()<<" passed\n"; return passed==cases.size()?0:1;
}
