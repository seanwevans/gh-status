⍝ GitHub Actions Build Monitor in APL
⍝ Uses GitHub CLI to fetch latest workflow run for repositories.
⍝ Usage: dyalogscript ghstatus.apl user1 user2 ...

⎕IO←0

GetArgs←{2⊃⎕NQ '.' 'GetCommandLineArgs'}

RepoList←{
    user←⍵
    data←⎕JSON ⎕CMD 'gh repo list ',user,' --limit 100 --json nameWithOwner'
    data['nameWithOwner']
}

RunStatus←{
    repo←⍵
    runs←⎕JSON ⎕CMD 'gh run list ',repo,' --limit 1 --json conclusion status'
    0=≢runs:'⚪'
    run←runs[0]
    stat←run['status']
    conc←run['conclusion']
    stat≡'queued':'⏳'
    stat≡'in_progress':'🏃'
    conc≡'success':'✅'
    conc≡'failure':'❌'
    conc≡'cancelled':'🚫'
    '❓'
}

ShowRepo←{
    repo←⍵
    emoji←RunStatus repo
    ⎕←emoji,' ',repo
}

main←{
    users←GetArgs
    repos←∊RepoList¨users
    ShowRepo¨repos
}

main ⍬
