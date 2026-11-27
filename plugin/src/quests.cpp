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

#include <string>

namespace
{
	constexpr std::ptrdiff_t kQuestFlags = 0x114;
	constexpr std::ptrdiff_t kObjCount = 0x148;
	constexpr std::ptrdiff_t kObjArray = 0x150;
	constexpr std::ptrdiff_t kObjState = 0x28;
	constexpr int            kStateDisplayed = 1;

	template <class T>
	T Read(const void* a_base, std::ptrdiff_t a_off)
	{
		return *reinterpret_cast<const T*>(reinterpret_cast<std::uintptr_t>(a_base) + a_off);
	}

	std::ptrdiff_t g_fullNameOffset = -1;  // -1 = not searched yet, 0 = not found

	const char* QuestName(const RE::TESForm* a_quest)
	{
		if (g_fullNameOffset < 0) {
			g_fullNameOffset = 0;
			const auto vtbl = RE::VTABLE::TESFullName[0].address();
			for (std::ptrdiff_t off = 0x30; off < kQuestFlags; off += 8) {
				if (Read<std::uintptr_t>(a_quest, off) == vtbl) {
					g_fullNameOffset = off;
					break;
				}
			}
			REX::INFO("quests: TESFullName component at +0x{:X}", g_fullNameOffset);
		}
		if (g_fullNameOffset == 0) {
			return nullptr;
		}
		const auto* full = reinterpret_cast<const RE::TESFullName*>(reinterpret_cast<std::uintptr_t>(a_quest) + g_fullNameOffset);
		return full->fullName.c_str();
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

namespace Quests
{
	std::vector<Ui::QuestEntry> CollectActive(std::size_t a_maxQuests)
	{
		std::vector<Ui::QuestEntry> result;
		auto*                       dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			return result;
		}
		auto& fa = dh->formArrays[std::to_underlying(RE::FormType::kQUST)];
		fa.lock.LockRead();
		for (std::uint32_t i = 0; i < fa.formArray.size() && result.size() < a_maxQuests; ++i) {
			const RE::TESForm* q = fa.formArray[i].get();
			if (!q || (Read<std::uint32_t>(q, kQuestFlags) & 1) == 0) {
				continue;
			}
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
			const char* name = QuestName(q);
			entry.name = Clean(name);
			if (entry.name.empty()) {
				entry.name = q->GetFormEditorID() ? q->GetFormEditorID() : "?";
			}
			result.push_back(std::move(entry));
		}
		fa.lock.UnlockRead();
		return result;
	}
}
