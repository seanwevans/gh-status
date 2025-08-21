use std::env;
use serde::Deserialize;

#[derive(Deserialize)]
struct Repo {
    name: String,
}

#[derive(Deserialize)]
struct RunsResponse {
    workflow_runs: Vec<Run>,
}

#[derive(Deserialize)]
struct Run {
    status: String,
    conclusion: Option<String>,
}

fn status_emoji(run: &Run) -> &'static str {
    match run.conclusion.as_deref() {
        Some("success") => "✅",
        Some("failure") => "❌",
        Some("cancelled") => "🚫",
        _ => match run.status.as_str() {
            "in_progress" => "🔄",
            "queued" => "⏳",
            _ => "❔",
        },
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let users: Vec<String> = env::args().skip(1).collect();
    if users.is_empty() {
        eprintln!("Usage: ghstatus-rs <user> [user2 ...]");
        std::process::exit(1);
    }

    for user in users {
        let repos: Vec<Repo> = ureq::get(&format!("https://api.github.com/users/{}/repos", user))
            .set("User-Agent", "ghstatus-rs")
            .call()?
            .into_json()?;

        for repo in repos {
            let url = format!(
                "https://api.github.com/repos/{}/{}/actions/runs?per_page=1",
                user, repo.name
            );
            let runs: RunsResponse = ureq::get(&url)
                .set("User-Agent", "ghstatus-rs")
                .call()?
                .into_json()?;

            if let Some(run) = runs.workflow_runs.first() {
                println!("{}/{} {}", user, repo.name, status_emoji(run));
            } else {
                println!("{}/{} ❔", user, repo.name);
            }
        }
    }
    Ok(())
}
