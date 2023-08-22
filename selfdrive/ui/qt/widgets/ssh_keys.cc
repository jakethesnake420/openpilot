#include "selfdrive/ui/qt/widgets/ssh_keys.h"

#include "common/params.h"
#include "selfdrive/ui/qt/api.h"
#include "selfdrive/ui/qt/widgets/input.h"

SshControl::SshControl() : ButtonControl(tr("SSH Keys"), "", tr("Warning: This grants SSH access to all public keys in your GitHub settings. Never enter a GitHub username other than your own. A comma employee will NEVER ask you to add their GitHub username.")) {
  QObject::connect(this, &ButtonControl::clicked, [=]() {
    if ((text() == tr("ADD")) || (text() == tr("EDIT"))) {
      QString usernamesInput = InputDialog::getText(tr("Enter GitHub usernames (comma or space separated)"), this);
      
      // Splitting the input string based on commas and spaces, and skipping empty parts.
      QStringList usernames = usernamesInput.split(QRegExp("[,\\s]+"), QString::SkipEmptyParts);
      if (usernames.length() > 0) {
        setText(tr("LOADING"));
        setEnabled(false);
        for (const QString &username : usernames) {
          if (!username.contains(QRegExp("^[a-zA-Z0-9-]+$"))) {
            ConfirmationDialog::alert(tr("Invalid username: '%1'. Usernames can only have alphanumeric characters and dash (-).").arg(username), this);
            return;
          }
          // Get SSH keys for each valid username
          getUserKeys(username);
        }
      }
    } else {
      params.remove("GithubUsername");
      params.remove("GithubSshKeys");
      refresh();
    }
  });

  refresh();
}

void SshControl::refresh() {
  QString param = QString::fromStdString(params.get("GithubSshKeys"));
  if (param.length()) {
    setValue(QString::fromStdString(params.get("GithubUsername")));
    setText(tr("EDIT"));
  } else {
    setValue("");
    setText(tr("ADD"));
  }
  setEnabled(true);
}

void SshControl::getUserKeys(const QString &username) {
  HttpRequest *request = new HttpRequest(this, false);
  QObject::connect(request, &HttpRequest::requestDone, [=](const QString &resp, bool success) {
    if (success) {
      if (!resp.isEmpty()) {
        // Store usernames and SSH keys
        // Here, for simplicity, we're appending the usernames and their SSH keys, but you might want a more structured storage.
        std::string existingUsernames = params.get("GithubUsername");
        std::string existingKeys = params.get("GithubSshKeys");
        
        if (!existingUsernames.empty()) {
          existingUsernames += ",";
          existingKeys += ",";
        }
        
        params.put("GithubUsername", (existingUsernames + username.toStdString()).c_str());
        params.put("GithubSshKeys", (existingKeys + resp.toStdString()).c_str());
      } else {
        ConfirmationDialog::alert(tr("Username '%1' has no keys on GitHub").arg(username), this);
      }
    } else {
      if (request->timeout()) {
        ConfirmationDialog::alert(tr("Request timed out"), this);
      } else {
        ConfirmationDialog::alert(tr("Username '%1' doesn't exist on GitHub").arg(username), this);
      }
    }

    refresh();
    request->deleteLater();
  });

  request->sendRequest("https://github.com/" + username + ".keys");
}
