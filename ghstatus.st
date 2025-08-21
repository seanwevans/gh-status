"GitHub Actions Build Monitor in Smalltalk"

Object subclass: #GHStatus
    instanceVariableNames: ''
    classVariableNames: ''
    poolDictionaries: ''
    category: 'GHStatus'.

GHStatus class >> statusIcons
    "Mapping from workflow status to emoji"
    ^ #(
        ('success' '✅')
        ('failure' '❌')
        ('cancelled' '🛑')
        ('skipped' '⏭️')
        ('timed_out' '⌛')
        ('action_required' '⛔')
        ('neutral' '⭕')
        ('stale' '🥖')
        ('in_progress' '🔁')
        ('queued' '📋')
        ('no_runs' '➖')
        ('completed' '➖')
        ('loading' '🌀')
        ('error' '⚠️')
        ('default' '➖')
    ) asDictionary.

GHStatus class >> iconFor: status
    status ifNil: [ ^ self statusIcons at: 'default' ].
    self statusIcons associationsDo: [:assoc |
        assoc key = 'default' ifFalse: [
            (status includesSubstring: assoc key)
                ifTrue: [ ^ assoc value ]]].
    ^ self statusIcons at: 'default'.

GHStatus class >> fetchReposFor: user
    | repos page |
    repos := OrderedCollection new.
    page := 1.
    [ | resp data |
        resp := ZnClient new
            url: 'https://api.github.com/users/', user, '/repos?per_page=100&type=public&page=', page asString;
            get.
        resp isSuccess ifFalse: [ ^ repos asArray ].
        data := NeoJSONReader fromString: resp contents.
        repos addAll: (data collect: [:each | each at: 'full_name']).
        data size < 100 ] whileFalse: [ page := page + 1 ].
    ^ repos asArray.

GHStatus class >> fetchStatusFor: repo
    | resp data run |
    resp := ZnClient new
        url: 'https://api.github.com/repos/', repo, '/actions/runs?per_page=1';
        get.
    resp isSuccess ifFalse: [ ^ 'error' ].
    data := NeoJSONReader fromString: resp contents.
    (data at: 'workflow_runs') isEmpty ifTrue: [ ^ 'no_runs' ].
    run := (data at: 'workflow_runs') first.
    (run at: 'conclusion') ifNil: [ ^ run at: 'status' ].
    ^ (run at: 'status'), ' ', (run at: 'conclusion').

GHStatus class >> printStatusForUsers: users
    | repos |
    repos := users flatCollect: [:u | self fetchReposFor: u].
    repos do: [:repo |
        | status icon |
        status := self fetchStatusFor: repo.
        icon := self iconFor: status.
        Transcript
            show: icon;
            show: ' ';
            show: repo;
            show: ' - ';
            show: status;
            cr ].

GHStatus class >> main
    | args |
    args := Smalltalk getArgs.
    args isEmpty ifTrue: [
        Transcript show: 'Usage: st ghstatus.st <user1> [user2 ...]'; cr.
        ^ self ].
    self printStatusForUsers: args.
