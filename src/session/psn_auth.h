#pragma once

#include <string>

namespace brp::session {

struct PsnAccount {
    std::string account_id;  // base64 of the 8-byte PSN account id
    std::string numeric_id;  // decimal user id returned by PSN
};

// PSN Remote Play OAuth login (the same public client used by chiaki-ng).
// The browser is opened at Sony's sign-in page. After the blank redirect page,
// the user pastes the final URL back into the app.
std::wstring PsnLoginUrl();
bool OpenPsnLoginPage(std::string& detail);
bool PsnCompleteLogin(const std::string& redirect_url_or_code, PsnAccount& out,
                      std::string& detail);

}  // namespace brp::session
