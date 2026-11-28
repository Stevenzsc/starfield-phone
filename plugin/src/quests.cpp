// Quest data for the Quests app. CommonLibSF's TESQuest is mostly padding, so this uses the layout found in
// Starfield.exe 1.16.244 (Ghidra; see docs/p0-findings.md):
//   Papyrus Quest.IsObjectiveDisplayed (FUN_1420CB700) -> objective lookup FUN_140D09190 and the quest loader
//   FUN_140D0B8D0 (QOBJ/FNAM/NNAM) and FUN_140D090A0 (adds an objective) show:
//   TESQuest +0x114  uint32 runtime flags, bit 0 = running
//            +0x148  uint32 objective count, +0x14C capacity, +0x150 BGSQuestObjective** (sorted by index)
//   BGSQuestObjective (0x30 bytes): +0x00 display text (BGSLocalizedString = BSFixedString, from NNAM),
//            +0x18 owner quest, +0x20 FNAM flags, +0x24 uint16 index, +0x28 int state
//            state: 0 dormant, 1 displayed (in progress), 2 completed, 3 completed+displayed, 4 failed, 5 failed+displayed
// The quest name: the TESFullName component, found once by scanning a quest for TESFullName's vtable.

#include "pch.h"
#include "quests.h"

#include <algorithm>
#include <string>

namespace
{
	constexpr std::ptrdiff_t kQuestFlags = 0x114;
	constexpr std::ptrdiff_t kObjCount = 0x148;
	constexpr std::ptrdiff_t kObjArray = 0x150;
	constexpr std::ptrdiff_t kObjState = 0x28;
	constexpr int            kStateDisplayed = 1;
	constexpr std::ptrdiff_t kDnamType = 0x110;      // DNAM bytes 8-11 (quest type?)
	constexpr std::ptrdiff_t kDnamPriority = 0x118;  // DNAM byte 4 (priority?)

	template <class T>
	T Read(const void* a_base, std::ptrdiff_t a_off)
	{
		return *reinterpret_cast<const T*>(reinterpret_cast<std::uintptr_t>(a_base) + a_off);
	}

	// The quest name is not inside TESQuest: the loader (FUN_140D0B8D0) puts FULL in a separate form component
	// (key DAT_145FDCE50). FUN_1405E3E30 (Address Library ID 47496) returns a form's full name through it, already
	// without a "<ID=...>" prefix.
	constexpr REL::ID kGetFullName{ 47496 };

	const char* QuestName(const RE::TESForm* a_quest)
	{
		using fn_t = const char* (*)(const RE::TESForm*);
		return reinterpret_cast<fn_t>(kGetFullName.address())(a_quest);
	}

	std::string Clean(const char* a_text)
	{
		std::string s = a_text ? a_text : "";
		// localized strings can start with an "<ID=xxxxxxxx>" prefix (see RE::BGSLocalizedString): drop it
		if (s.rfind("<ID=", 0) == 0) {
			const auto end = s.find('>');
			s = end == std::string::npos ? std::string{} : s.substr(end + 1);
		}
		// show text-replacement tags like <Alias=Target> (filled in by the game at display time) as "…"
		std::string out;
		bool        inTag = false;
		for (char c : s) {
			if (c == '<') {
				inTag = true;
				out += "…";
				continue;
			}
			if (c == '>' && inTag) {
				inTag = false;
				continue;
			}
			if (!inTag) {
				out += c;
			}
		}
		return out;
	}
}

namespace
{
	// All forms by ID (TESForm::LookupByID, FUN_1405DE3B0, reads it): Address Library ID 883341 holds a pointer to a
	// hash map; +0x10 entry array (0x18 bytes each: u32 form ID at +0, TESForm* at +8), +0x18 capacity,
	// +0x30 BSReadWriteLock. CommonLibSF's TESDataHandler::formArrays looked empty for quests in this version.
	constexpr REL::ID kAllFormsMap{ 883341 };
	constexpr int     kQuestType = 91;  // form type byte (+0x2E) of MQ101, matches CommonLibSF's kQUST

	std::vector<const RE::TESForm*> g_quests;  // every quest form, found once
	bool                            g_scanned = false;

	void ScanQuests()
	{
		g_scanned = true;
		auto* map = *reinterpret_cast<std::uint8_t**>(kAllFormsMap.address());
		if (!map) {
			REX::WARN("quests: form map not ready");
			g_scanned = false;
			return;
		}
		auto* lock = reinterpret_cast<RE::BSReadWriteLock*>(map + 0x30);
		lock->LockRead();
		const auto* entries = *reinterpret_cast<const std::uint8_t* const*>(map + 0x10);
		const auto  capacity = *reinterpret_cast<const std::uint64_t*>(map + 0x18);
		std::uint64_t forms = 0;
		for (std::uint64_t i = 0; entries && i < capacity && i < (1ull << 26); ++i) {
			const auto* e = entries + i * 0x18;
			const auto* form = *reinterpret_cast<const RE::TESForm* const*>(e + 8);
			if (!form || *reinterpret_cast<const std::uint32_t*>(e) != form->GetFormID()) {
				continue;  // empty slot
			}
			++forms;
			if (Read<std::uint8_t>(form, 0x2E) == kQuestType) {
				g_quests.push_back(form);
			}
		}
		lock->UnlockRead();
		REX::INFO("quests: scanned {} forms (map capacity {}), {} quests", forms, capacity, g_quests.size());
	}
}

namespace Quests
{
	std::vector<Ui::QuestEntry> CollectActive(std::size_t a_maxQuests)
	{
		std::vector<Ui::QuestEntry> result;
		if (!g_scanned) {
			ScanQuests();
		}
		struct Keyed
		{
			std::uint32_t      type;
			std::uint8_t       priority;
			const RE::TESForm* form;
			Ui::QuestEntry     entry;
		};
		std::vector<Keyed> keyed;
		for (const RE::TESForm* q : g_quests) {
			const auto count = Read<std::uint32_t>(q, kObjCount);
			const auto objs = Read<const void* const*>(q, kObjArray);
			if (!objs || count == 0 || count > 1024) {
				continue;
			}
			Ui::QuestEntry entry;
			for (std::uint32_t k = 0; k < count; ++k) {
				const void* o = objs[k];
				if (!o || Read<int>(o, kObjState) != kStateDisplayed) {
					continue;
				}
				const auto& text = *reinterpret_cast<const RE::BSFixedString*>(o);
				auto        t = Clean(text.c_str());
				if (!t.empty()) {
					entry.objectives.push_back(std::move(t));
				}
			}
			if (entry.objectives.empty()) {
				continue;
			}
			entry.name = Clean(QuestName(q));
			if (entry.name.empty()) {
				entry.name = q->GetFormEditorID() ? q->GetFormEditorID() : "?";
			}
			keyed.push_back({ Read<std::uint32_t>(q, kDnamType), Read<std::uint8_t>(q, kDnamPriority), q, std::move(entry) });
		}
		// quest type ascending, then priority descending (to be confirmed in game: see the log below)
		std::stable_sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) {
			return a.type != b.type ? a.type < b.type : a.priority > b.priority;
		});
		static bool logged = false;
		for (auto& k : keyed) {
			if (!logged) {
				REX::INFO("quests: type {} priority {} {:08X} '{}' = '{}'", k.type, k.priority, k.form->GetFormID(),
					k.form->GetFormEditorID() ? k.form->GetFormEditorID() : "", k.entry.name);
			}
			if (result.size() < a_maxQuests) {
				result.push_back(std::move(k.entry));
			}
		}
		logged = logged || !keyed.empty();
		return result;
	}
}
