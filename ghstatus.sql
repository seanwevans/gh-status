-- PL/pgSQL implementation of GitHub Actions Build Monitor
-- Requires the pgsql-http extension: https://github.com/pramsey/pgsql-http

CREATE EXTENSION IF NOT EXISTS http;

-- Fetch latest workflow runs for each repository owned by the given GitHub usernames.
-- Returns one row per repository with the workflow conclusion and a link to the run.
CREATE OR REPLACE FUNCTION ghstatus_latest_runs(usernames text[])
RETURNS TABLE(
    username    text,
    repository  text,
    status      text,
    html_url    text
) LANGUAGE plpgsql AS $$
DECLARE
    user_name text;
    repos jsonb;
    repo jsonb;
    run_resp http_response;
    run jsonb;
    api_base CONSTANT text := 'https://api.github.com';
BEGIN
    FOREACH user_name IN ARRAY usernames LOOP
        -- list repositories for the user
        SELECT content::jsonb INTO repos
        FROM http_get(api_base || '/users/' || user_name || '/repos');

        FOR repo IN SELECT * FROM jsonb_array_elements(repos) LOOP
            -- fetch the most recent workflow run for the repository
            SELECT * INTO run_resp
            FROM http_get(api_base || '/repos/' || repo->>'full_name' || '/actions/runs?per_page=1');

            run := run_resp.content::jsonb -> 'workflow_runs' -> 0;
            username   := user_name;
            repository := repo->>'name';
            status     := COALESCE(run->>'conclusion', run->>'status');
            html_url   := run->>'html_url';
            RETURN NEXT;
        END LOOP;
    END LOOP;
END;
$$;
