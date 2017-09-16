/*
 * Copyright (C) 2016~2016 by CSSlayer
 * wengxt@gmail.com
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of the
 * License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; see the file COPYING. If not,
 * see <http://www.gnu.org/licenses/>.
 */

#include "keyboard.h"
#include "chardata.h"
#include "config.h"
#include "fcitx-config/iniparser.h"
#include "fcitx-utils/cutf8.h"
#include "fcitx-utils/i18n.h"
#include "fcitx-utils/standardpath.h"
#include "fcitx-utils/stringutils.h"
#include "fcitx-utils/utf8.h"
#include "fcitx/inputcontext.h"
#include "fcitx/inputcontextmanager.h"
#include "fcitx/inputcontextproperty.h"
#include "fcitx/inputpanel.h"
#include "fcitx/instance.h"
#include "notifications_public.h"
#include "spell_public.h"
#include "xcb_public.h"
#include <cstring>
#include <fcntl.h>
#include <libintl.h>
#include <fmt/format.h>

const char imNamePrefix[] = "keyboard-";
const int imNamePrefixLength = sizeof(imNamePrefix) - 1;
#define FCITX_KEYBOARD_MAX_BUFFER 20

namespace fcitx {

static std::string findBestLanguage(const IsoCodes &isocodes,
                                    const std::string &hint,
                                    const std::vector<std::string> &languages) {
    /* score:
     * 1 -> first one
     * 2 -> match 2
     * 3 -> match three
     */
    const IsoCodes639Entry *bestEntry = nullptr;
    int bestScore = 0;
    for (auto &language : languages) {
        auto entry = isocodes.entry(language);
        if (!entry) {
            continue;
        }

        auto langCode = entry->iso_639_1_code;
        if (langCode.empty()) {
            langCode = entry->iso_639_2T_code;
        }

        if (langCode.empty()) {
            langCode = entry->iso_639_2B_code;
        }

        if (langCode.empty()) {
            continue;
        }

        if (langCode.size() != 2 && langCode.size() != 3) {
            continue;
        }

        int score = 1;
        auto len = langCode.size();
        while (len >= 2) {
            if (strncasecmp(hint.c_str(), langCode.c_str(), len) == 0) {
                score = len;
                break;
            }

            len--;
        }

        if (bestScore < score) {
            bestEntry = entry;
            bestScore = score;
        }
    }
    if (bestEntry) {
        if (!bestEntry->iso_639_1_code.empty()) {
            return bestEntry->iso_639_1_code;
        }
        if (!bestEntry->iso_639_2T_code.empty()) {
            return bestEntry->iso_639_2T_code;
        }
        return bestEntry->iso_639_2B_code;
    }
    return {};
}

std::pair<std::string, std::string> layoutFromName(const std::string &s) {
    auto pos = s.find('-', imNamePrefixLength);
    if (pos == std::string::npos) {
        return {s.substr(imNamePrefixLength), ""};
    }
    return {s.substr(imNamePrefixLength, pos - imNamePrefixLength),
            s.substr(pos + 1)};
}

KeyboardEngine::KeyboardEngine(Instance *instance) : instance_(instance) {
    registerDomain("xkeyboard-config", XKEYBOARDCONFIG_DATADIR "/locale");
    isoCodes_.read(ISOCODES_ISO639_XML, ISOCODES_ISO3166_XML);
    auto xcb = instance_->addonManager().addon("xcb");
    std::string rule;
    if (xcb) {
        auto rules = xcb->call<IXCBModule::xkbRulesNames>("");
        if (!rules[0].empty()) {
            rule = rules[0];
            if (rule[0] == '/') {
                rule += ".xml";
            } else {
                rule = XKEYBOARDCONFIG_XKBBASE "/rules/" + rule + ".xml";
            }
            ruleName_ = rule;
        }
    }
    if (rule.empty() || !xkbRules_.read(rule)) {
        rule = XKEYBOARDCONFIG_XKBBASE "/rules/" DEFAULT_XKB_RULES ".xml";
        xkbRules_.read(rule);
        ruleName_ = DEFAULT_XKB_RULES;
    }

    instance_->inputContextManager().registerProperty("keyboardState",
                                                      &factory_);
    reloadConfig();
}

KeyboardEngine::~KeyboardEngine() {}

std::vector<InputMethodEntry> KeyboardEngine::listInputMethods() {
    std::vector<InputMethodEntry> result;
    for (auto &p : xkbRules_.layoutInfos()) {
        auto &layoutInfo = p.second;
        auto language = findBestLanguage(isoCodes_, layoutInfo.description,
                                         layoutInfo.languages);
        auto description = fmt::format(_("Keyboard - {0}"),
                               D_("xkeyboard-config", layoutInfo.description));
        auto uniqueName = imNamePrefix + layoutInfo.name;
        result.push_back(std::move(
            InputMethodEntry(uniqueName, description, language, "keyboard")
                .setIcon("kbd")
                .setLabel(layoutInfo.name)));
        for (auto &variantInfo : layoutInfo.variantInfos) {
            auto language = findBestLanguage(isoCodes_, variantInfo.description,
                                             variantInfo.languages.size()
                                                 ? variantInfo.languages
                                                 : layoutInfo.languages);
            auto description = stringutils::join(
                {_("Keyboard"), " - ",
                 D_("xkeyboard-config", layoutInfo.description), " - ",
                 D_("xkeyboard-config", variantInfo.description)},
                "");
            auto uniqueName =
                imNamePrefix + layoutInfo.name + "-" + variantInfo.name;
            result.push_back(std::move(
                InputMethodEntry(uniqueName, description, language, "keyboard")
                    .setIcon("kbd")
                    .setLabel(layoutInfo.name)));
        }
    }
    return result;
}

void KeyboardEngine::reloadConfig() {
    auto &standardPath = StandardPath::global();
    auto file = standardPath.open(StandardPath::Type::PkgConfig,
                                  "conf/keyboard.conf", O_RDONLY);
    RawConfig config;
    readFromIni(config, file.fd());

    config_.load(config);
    selectionKeys_.clear();
    KeySym syms[] = {
        FcitxKey_1, FcitxKey_2, FcitxKey_3, FcitxKey_4, FcitxKey_5,
        FcitxKey_6, FcitxKey_7, FcitxKey_8, FcitxKey_9, FcitxKey_0,
    };

    KeyStates states;
    switch (config_.chooseModifier.value()) {
    case ChooseModifier::Alt:
        states = KeyState::Alt;
        break;
    case ChooseModifier::Control:
        states = KeyState::Ctrl;
        break;
    case ChooseModifier::Super:
        states = KeyState::Super;
        break;
    default:
        break;
    }

    for (auto sym : syms) {
        selectionKeys_.emplace_back(sym, states);
    }
}

static inline bool isValidSym(const Key &key) {
    if (key.states())
        return false;

    return validSyms.count(key.sym());
}

static inline bool isValidCharacter(uint32_t c) {
    if (c == 0 || c == FCITX_INVALID_COMPOSE_RESULT)
        return false;

    return validChars.count(c);
}

static KeyList FCITX_HYPHEN_APOS = Key::keyListFromString("minus apostrophe");

void KeyboardEngine::keyEvent(const InputMethodEntry &entry, KeyEvent &event) {
    // FIXME use entry to get layout info
    FCITX_UNUSED(entry);

    // by pass all key release
    if (event.isRelease()) {
        return;
    }

    // and by pass all modifier
    if (event.key().isModifier()) {
        return;
    }
    auto inputContext = event.inputContext();
    auto state = inputContext->propertyFor(&factory_);

    // check compose first.
    auto compose = instance_->processCompose(inputContext, event.key().sym());

    // compose is invalid, ignore it.
    if (compose == FCITX_INVALID_COMPOSE_RESULT) {
        return event.filterAndAccept();
    }

    // check the spell trigger key
    if (event.key().checkKeyList(config_.hintTrigger.value()) && spell() &&
        spell()->call<ISpell::checkDict>(entry.languageCode())) {
        state->enableWordHint_ = !state->enableWordHint_;
        commitBuffer(inputContext);
        if (notifications()) {
            notifications()->call<INotifications::showTip>(
                "fcitx-keyboard-hint", "fcitx", "tools-check-spelling",
                _("Spell hint"),
                state->enableWordHint_ ? _("Spell hint is enabled.")
                                       : _("Spell hint is disabled."),
                -1);
        }
        return event.filterAndAccept();
    }

    do {
        // no spell hint enabled, ignore
        if (!state->enableWordHint_) {
            break;
        }
        // no supported dictionary
        if (!spell() ||
            !spell()->call<ISpell::checkDict>(entry.languageCode())) {
            break;
        }

        // check if we can select candidate.
        if (inputContext->inputPanel().candidateList()) {
            int idx = event.key().keyListIndex(selectionKeys_);
            if (idx >= 0 &&
                idx < inputContext->inputPanel().candidateList()->size()) {
                event.filterAndAccept();
                inputContext->inputPanel()
                    .candidateList()
                    ->candidate(idx)
                    ->select(inputContext);
                return;
            }
        }

        auto &buffer = state->buffer_;
        bool validCharacter = isValidCharacter(compose);
        bool validSym = isValidSym(event.key());

        // check for valid character
        if (validCharacter || event.key().isSimple() || validSym) {
            if (validCharacter || event.key().isLAZ() || event.key().isUAZ() ||
                validSym || (!buffer.empty() &&
                             event.key().checkKeyList(FCITX_HYPHEN_APOS))) {
                if (compose) {
                    buffer.type(compose);
                } else {
                    buffer.type(Key::keySymToUnicode(event.key().sym()));
                }

                event.filterAndAccept();
                if (buffer.size() >= FCITX_KEYBOARD_MAX_BUFFER) {
                    inputContext->commitString(buffer.userInput());
                    resetState(inputContext);
                    return;
                }

                return updateCandidate(entry, inputContext);
            }
        } else {
            if (event.key().check(FcitxKey_BackSpace)) {
                if (buffer.backspace()) {
                    event.filterAndAccept();
                    return updateCandidate(entry, inputContext);
                }
            }
        }

        // if we reach here, just commit and discard buffer.
        commitBuffer(inputContext);
    } while (0);

    // and now we want to forward key.
    if (compose) {
        auto composeString = utf8::UCS4ToUTF8(compose);
        event.filterAndAccept();
        inputContext->commitString(composeString);
    }
}

void KeyboardEngine::commitBuffer(InputContext *inputContext) {
    auto state = inputContext->propertyFor(&factory_);
    auto &buffer = state->buffer_;
    if (!buffer.empty()) {
        inputContext->commitString(buffer.userInput());
        resetState(inputContext);
        inputContext->inputPanel().reset();
        inputContext->updatePreedit();
        inputContext->updateUserInterface(UserInterfaceComponent::InputPanel);
    }
}

class KeyboardCandidateWord : public CandidateWord {
public:
    KeyboardCandidateWord(KeyboardEngine *engine, Text text)
        : CandidateWord(std::move(text)), engine_(engine) {}

    void select(InputContext *inputContext) const override {
        auto commit = text().toString();
        inputContext->inputPanel().reset();
        inputContext->updatePreedit();
        inputContext->updateUserInterface(UserInterfaceComponent::InputPanel);
        inputContext->commitString(commit);
        engine_->resetState(inputContext);
    }

private:
    KeyboardEngine *engine_;
};

void KeyboardEngine::updateCandidate(const InputMethodEntry &entry,
                                     InputContext *inputContext) {
    auto state = inputContext->propertyFor(&factory_);
    auto results = spell()->call<ISpell::hint>(entry.languageCode(),
                                               state->buffer_.userInput(),
                                               config_.pageSize.value());
    auto candidateList = new CommonCandidateList;
    for (const auto &result : results) {
        candidateList->append(new KeyboardCandidateWord(this, Text(result)));
    }
    candidateList->setSelectionKey(selectionKeys_);
    Text preedit(state->buffer_.userInput());
    if (state->buffer_.size()) {
        preedit.setCursor(state->buffer_.cursorByChar());
    }
    inputContext->inputPanel().setClientPreedit(preedit);
    if (!inputContext->capabilityFlags().test(CapabilityFlag::Preedit)) {
        inputContext->inputPanel().setPreedit(preedit);
    }
    inputContext->inputPanel().setCandidateList(candidateList);
    inputContext->updatePreedit();
    inputContext->updateUserInterface(UserInterfaceComponent::InputPanel);
}

void KeyboardEngine::resetState(InputContext *inputContext) {
    auto state = inputContext->propertyFor(&factory_);
    state->reset();
    instance_->resetCompose(inputContext);
}

void KeyboardEngine::reset(const InputMethodEntry &, InputContextEvent &event) {
    auto inputContext = event.inputContext();
    inputContext->inputPanel().reset();
    inputContext->updatePreedit();
    inputContext->updateUserInterface(UserInterfaceComponent::InputPanel);
}
}
