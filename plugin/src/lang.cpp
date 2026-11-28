// Phone UI translations (Chinese source text -> English).

#include "lang.h"

#include <atomic>
#include <cstdio>
#include <string_view>
#include <unordered_map>

namespace
{
	std::atomic_int  g_mode{ 2 };  // English unless the ini says otherwise
	std::atomic_bool g_gameEnglish{ false };

	const std::unordered_map<std::string_view, const char*>& Table()
	{
		static const std::unordered_map<std::string_view, const char*> t = {
			// apps
			{ "消息", "Messages" },
			{ "任务", "Quests" },
			{ "角色", "Character" },
			{ "飞船", "Ship" },
			{ "星图", "Star Map" },
			{ "日历", "Calendar" },
			{ "相机", "Camera" },
			{ "相册", "Gallery" },
			{ "音乐", "Music" },
			{ "联系人", "Contacts" },
			{ "前哨站", "Outposts" },
			{ "扫描仪", "Scanner" },
			{ "设置", "Settings" },
			// button prompts
			{ "选择", "Select" },
			{ "打开", "Open" },
			{ "调整", "Adjust" },
			{ "退格", "Bksp" },
			{ "返回", "Back" },
			{ "滚动", "Scroll" },
			{ "详情", "Details" },
			{ "翻看", "Browse" },
			{ "删除", "Delete" },
			{ "确认删除", "Confirm" },
			{ "全部清除", "Clear all" },
			{ "确认清除", "Confirm" },
			{ "呼叫", "Call" },
			{ "叫过来", "Come here" },
			{ "挂断", "Hang up" },
			{ "拍照", "Shoot" },
			{ "拍照模式", "Photo mode" },
			{ "追踪", "Track" },
			{ "取消追踪", "Untrack" },
			{ "切歌", "Track" },
			{ "暂停", "Pause" },
			{ "播放", "Play" },
			{ "音量", "Volume" },
			// status bar / common
			{ "星联", "SolNet" },
			{ "读取中…", "Loading…" },
			{ "加载中…", "Loading…" },
			{ "即将推出", "Coming soon" },
			{ "暂无数据", "No data" },
			{ "开", "On" },
			{ "关", "Off" },
			{ "，", ", " },
			{ "未知", "Unknown" },
			// settings
			{ "屏幕亮度", "Brightness" },
			{ "打开手机", "Open key" },
			{ "按键音效", "Key sounds" },
			{ "手柄震动", "Rumble" },
			{ "语言", "Language" },
			{ "自动", "Auto" },
			{ "中文", "中文" },
			// camera / gallery
			{ "取景中…", "Framing…" },
			{ "已保存", "Saved" },
			{ "已保存 · ", "Saved · " },
			{ "保存失败", "Save failed" },
			{ "全部", "All" },
			{ "手机", "Phone" },
			{ "照片", " photos" },
			{ "全部照片", "All photos" },
			{ "未知位置", "Unknown location" },
			{ "没有位置记录的照片", "Photos without a location" },
			{ "%d 张", "%d photos" },
			{ "还没有照片", "No photos yet" },
			{ "手机相机或拍照模式拍的照片会出现在这里", "Phone camera and photo mode shots appear here" },
			{ "这个相册是空的", "This album is empty" },
			{ "无法打开", "Cannot open" },
			{ "再按 Enter 删除（移到回收站）", "Enter again: delete (Recycle Bin)" },
			{ "位置未知", "Location unknown" },
			{ "GPS 无信号", "GPS no signal" },
			{ "北纬", "N" },
			{ "南纬", "S" },
			{ "东经", "E" },
			{ "西经", "W" },
			{ "照片已保存（位置未知）", "Photo saved (location unknown)" },
			{ "照片已记录位置：", "Photo location saved: " },
			// quests / messages
			{ "主线", "Main" },
			{ "派系", "Faction" },
			{ "支线", "Side" },
			{ "任务板", "Missions" },
			{ "活动", "Activities" },
			{ "其他", "Other" },
			{ "新目标：", "New: " },
			{ "已完成：", "Completed: " },
			{ "已失败：", "Failed: " },
			{ "没有进行中的任务", "No active quests" },
			{ "任务详情", "Quest details" },
			{ "◆ 正在追踪", "◆ Tracking" },
			{ "未追踪", "Not tracked" },
			{ "%d 条", "%d" },
			{ "没有消息", "No messages" },
			{ "任务目标更新、升级会出现在这里", "Quest updates and level-ups appear here" },
			{ "升级到 ", "Reached level " },
			{ " 级", "" },
			// character / ship / map / outposts
			{ "姓名", "Name" },
			{ "等级", "Level" },
			{ "经验", "XP" },
			{ "信用点", "Credits" },
			{ "状态", "Status" },
			{ "生命", "Health" },
			{ "氧气", "Oxygen" },
			{ "负重上限", "Carry limit" },
			{ "名称", "Name" },
			{ "位置", "Location" },
			{ "船体", "Hull" },
			{ "护盾", "Shield" },
			{ "跃迁燃料", "Grav fuel" },
			{ "性能", "Performance" },
			{ "跃迁距离", "Jump range" },
			{ " 光年", " LY" },
			{ "货舱容量", "Cargo" },
			{ "反应堆", "Reactor" },
			{ "船员", "Crew" },
			{ "乘客舱位", "Passenger slots" },
			{ "没有家园飞船", "No home ship" },
			{ "当前位置", "Current location" },
			{ "星系", "System" },
			{ "星球", "Planet" },
			{ "太空", "Space" },
			{ "地点", "Place" },
			{ "在飞船上", "Aboard ship" },
			{ "太空中", "In space" },
			{ "地面", "On the ground" },
			{ "无信号", "No signal" },
			{ "家园飞船", "Home ship" },
			{ "停靠", "Docked at" },
			{ "前哨站 ", "Outpost " },
			{ "距离", "Distance" },
			{ "没有找到前哨站", "No outposts found" },
			// contacts
			{ "%d 人", "%d" },
			{ "没有船员", "No crew" },
			{ "分配到家园飞船的船员会出现在这里", "Ship crew and companions appear here" },
			{ "附近 ", "Nearby " },
			{ "已阵亡", "Dead" },
			{ "无法接通", "Unreachable" },
			{ "通话中…", "In call…" },
			{ "正在请求…", "Requesting…" },
			{ "已呼叫，正在赶来", "Called, on the way" },
			{ "“收到，马上到。”", "\"Copy that, on my way.\"" },
			// scanner
			{ "范围 %.0f m", "Range %.0f m" },
			{ "敌对 %d", "Hostile %d" },
			{ "扫描中…", "Scanning…" },
			{ "附近没有发现生命体", "No life signs nearby" },
			{ "前方", "Ahead" },
			{ "右前", "Front right" },
			{ "右侧", "Right" },
			{ "右后", "Rear right" },
			{ "后方", "Behind" },
			{ "左后", "Rear left" },
			{ "左侧", "Left" },
			{ "左前", "Front left" },
			// music
			{ "无法播放 ", "Cannot play " },
			{ "没有音乐", "No music" },
			{ "把 mp3 / wav / wma 文件放进这个文件夹，再打开音乐：", "Put mp3 / wav / wma files in this folder, then reopen Music:" },
			{ "文档\\My Games\\Starfield\\PhoneMusic", "Documents\\My Games\\Starfield\\PhoneMusic" },
			{ "未播放", "Stopped" },
			{ "正在播放", "Playing" },
			{ "已暂停", "Paused" },
			{ "音量 %d%%", "Volume %d%%" },
			{ "音量 100%", "Volume 100%" },
			{ "接下来", "Up next" },
			// pose tuner (dev keys)
			{ "锁骨", "Clavicle" },
			{ "上臂", "Upper arm" },
			{ "前臂", "Forearm" },
			{ "手腕", "Wrist" },
			{ "小键盘调整：", "Numpad tuning: " },
			{ "已重置", "Reset" },
			{ "姿势已保存", "Pose saved" },
		};
		return t;
	}

	constexpr const char* kMonthsEn[] = { "January", "February", "March", "April", "May", "June", "July", "August",
		"September", "October", "November", "December" };
	constexpr const char* kWeekZh[] = { "日", "一", "二", "三", "四", "五", "六" };
	constexpr const char* kWeekZhLong[] = { "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六" };
	constexpr const char* kWeekEn[] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
	constexpr const char* kWeekEnLong[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
}

namespace Lang
{
	void SetMode(int a_mode) { g_mode = (a_mode < 0 || a_mode > 2) ? 2 : a_mode; }
	int  Mode() { return g_mode.load(); }

	const char* ModeName(int a_mode)
	{
		switch (a_mode) {
		case 1: return "中文";
		case 2: return "English";
		default: return Tr("自动");
		}
	}

	void SetGameEnglish(bool a_english) { g_gameEnglish = a_english; }

	bool English()
	{
		const int m = g_mode.load();
		return m == 2 || (m == 0 && g_gameEnglish.load());
	}

	std::string Date(unsigned a_year, unsigned a_m0, unsigned a_day)
	{
		char buf[64];
		if (English()) std::snprintf(buf, sizeof(buf), "%.3s %u, %u", kMonthsEn[a_m0 % 12], a_day, a_year);
		else std::snprintf(buf, sizeof(buf), "%u年%u月%u日", a_year, a_m0 + 1, a_day);
		return buf;
	}

	std::string MonthDay(unsigned a_m0, unsigned a_day)
	{
		char buf[32];
		if (English()) std::snprintf(buf, sizeof(buf), "%.3s %u", kMonthsEn[a_m0 % 12], a_day);
		else std::snprintf(buf, sizeof(buf), "%u月%u日", a_m0 + 1, a_day);
		return buf;
	}

	std::string Month(unsigned a_m0)
	{
		char buf[32];
		if (English()) std::snprintf(buf, sizeof(buf), "%s", kMonthsEn[a_m0 % 12]);
		else std::snprintf(buf, sizeof(buf), "%u月", a_m0 + 1);
		return buf;
	}

	std::string Year(unsigned a_year)
	{
		char buf[16];
		std::snprintf(buf, sizeof(buf), English() ? "%u" : "%u年", a_year);
		return buf;
	}

	const char* WeekdayShort(int a_wd) { return (English() ? kWeekEn : kWeekZh)[((a_wd % 7) + 7) % 7]; }
	const char* WeekdayLong(int a_wd) { return (English() ? kWeekEnLong : kWeekZhLong)[((a_wd % 7) + 7) % 7]; }
}

const char* Tr(const char* a_zh)
{
	if (!a_zh || !Lang::English()) {
		return a_zh;
	}
	const auto& t = Table();
	const auto  it = t.find(a_zh);
	return it == t.end() ? a_zh : it->second;
}
