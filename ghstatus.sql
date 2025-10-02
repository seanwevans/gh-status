-- PL/pgSQL implementation of GitHub Actions Build Monitor
-- Requires the pgsql-http extension: https://github.com/pramsey/pgsql-http
-- HTTP requests that do not return JSON (or return non-200 responses) are skipped with NOTICEs
-- so a single bad API call will not abort the entire query.

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
    repo_resp http_response;
    run_resp http_response;
    run jsonb;
    api_base CONSTANT text := 'https://api.github.com';
BEGIN
    FOREACH user_name IN ARRAY usernames LOOP
        -- list repositories for the user
        SELECT * INTO repo_resp
        FROM http_get(api_base || '/users/' || user_name || '/repos');

        IF NOT FOUND THEN
            RAISE NOTICE 'Skipping repositories for %: no response returned', user_name;
            CONTINUE;
        END IF;

        IF repo_resp.status <> 200 OR repo_resp.content_type IS NULL OR repo_resp.content_type NOT LIKE 'application/json%' THEN
            RAISE NOTICE 'Skipping repositories for %: unexpected response (status %, content_type %)',
                user_name, repo_resp.status, repo_resp.content_type;
            CONTINUE;
        END IF;

        BEGIN
            repos := repo_resp.content::jsonb;
        EXCEPTION WHEN others THEN
            RAISE NOTICE 'Skipping repositories for %: unable to parse JSON payload', user_name;
            CONTINUE;
        END;

        IF repos IS NULL OR jsonb_typeof(repos) <> 'array' THEN
            RAISE NOTICE 'Skipping repositories for %: payload was not a JSON array', user_name;
            CONTINUE;
        END IF;

        FOR repo IN SELECT * FROM jsonb_array_elements(repos) LOOP
            -- fetch the most recent workflow run for the repository
            SELECT * INTO run_resp
            FROM http_get(api_base || '/repos/' || repo->>'full_name' || '/actions/runs?per_page=1');

            IF NOT FOUND THEN
                RAISE NOTICE 'Skipping workflow runs for %/%: no response returned', user_name, repo->>'name';
                CONTINUE;
            END IF;

            IF run_resp.status <> 200 OR run_resp.content_type IS NULL OR run_resp.content_type NOT LIKE 'application/json%' THEN
                RAISE NOTICE 'Skipping workflow runs for %/%: unexpected response (status %, content_type %)',
                    user_name, repo->>'name', run_resp.status, run_resp.content_type;
                CONTINUE;
            END IF;

            BEGIN
                run := run_resp.content::jsonb -> 'workflow_runs' -> 0;
            EXCEPTION WHEN others THEN
                RAISE NOTICE 'Skipping workflow runs for %/%: unable to parse JSON payload',
                    user_name, repo->>'name';
                CONTINUE;
            END;

            IF run IS NULL THEN
                RAISE NOTICE 'Skipping %/%: no workflow runs found', user_name, repo->>'name';
                CONTINUE;
            END IF;

            username   := user_name;
            repository := repo->>'name';
            status     := COALESCE(run->>'conclusion', run->>'status');
            html_url   := run->>'html_url';
            RETURN NEXT;
        END LOOP;
    END LOOP;
END;
$$;
