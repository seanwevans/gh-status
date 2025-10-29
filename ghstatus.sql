-- PL/pgSQL implementation of GitHub Actions Build Monitor
-- Requires the pgsql-http extension: https://github.com/pramsey/pgsql-http
-- HTTP requests that do not return JSON (or return non-200 responses) are skipped with NOTICEs
-- so a single bad API call will not abort the entire query.

CREATE EXTENSION IF NOT EXISTS http;

-- Fetch latest workflow runs for each repository owned by the given GitHub usernames.
-- Returns one row per repository with the workflow conclusion and a link to the run.
CREATE OR REPLACE FUNCTION ghstatus_latest_runs(usernames text[], github_token text DEFAULT NULL)
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
    per_page CONSTANT integer := 100;
    page integer;
    repo_count integer;
    auth_headers http_header[];
    header http_header;
    has_link_header boolean;
    has_next_page boolean;
BEGIN
    IF github_token IS NOT NULL THEN
        auth_headers := http_headers('Authorization', 'Bearer ' || github_token);
    END IF;

    FOREACH user_name IN ARRAY usernames LOOP
        page := 1;

        repo_page_loop:
        LOOP
            -- list repositories for the user
            IF auth_headers IS NOT NULL THEN
                SELECT * INTO repo_resp
                FROM http_get(api_base || '/users/' || user_name || '/repos?per_page=' || per_page || '&page=' || page, auth_headers);
            ELSE
                SELECT * INTO repo_resp
                FROM http_get(api_base || '/users/' || user_name || '/repos?per_page=' || per_page || '&page=' || page);
            END IF;

            IF NOT FOUND THEN
                RAISE NOTICE 'Skipping repositories for %: no response returned', user_name;
                EXIT repo_page_loop;
            END IF;

            IF repo_resp.status <> 200 OR repo_resp.content_type IS NULL OR repo_resp.content_type NOT LIKE 'application/json%' THEN
                RAISE NOTICE 'Skipping repositories for %: unexpected response (status %, content_type %)',
                    user_name, repo_resp.status, repo_resp.content_type;
                EXIT repo_page_loop;
            END IF;

            BEGIN
                repos := repo_resp.content::jsonb;
            EXCEPTION WHEN others THEN
                RAISE NOTICE 'Skipping repositories for %: unable to parse JSON payload', user_name;
                EXIT repo_page_loop;
            END;

            IF repos IS NULL OR jsonb_typeof(repos) <> 'array' THEN
                RAISE NOTICE 'Skipping repositories for %: payload was not a JSON array', user_name;
                EXIT repo_page_loop;
            END IF;

            repo_count := jsonb_array_length(repos);

            IF repo_count IS NULL OR repo_count = 0 THEN
                EXIT repo_page_loop;
            END IF;

            FOR repo IN SELECT * FROM jsonb_array_elements(repos) LOOP
                -- fetch the most recent workflow run for the repository
                IF auth_headers IS NOT NULL THEN
                    SELECT * INTO run_resp
                    FROM http_get(api_base || '/repos/' || repo->>'full_name' || '/actions/runs?per_page=1', auth_headers);
                ELSE
                    SELECT * INTO run_resp
                    FROM http_get(api_base || '/repos/' || repo->>'full_name' || '/actions/runs?per_page=1');
                END IF;

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

            has_link_header := false;
            has_next_page := false;

            IF repo_resp.headers IS NOT NULL THEN
                FOREACH header IN ARRAY repo_resp.headers LOOP
                    IF lower(header.field) = 'link' THEN
                        has_link_header := true;
                        IF position('rel="next"' IN header.value) > 0 THEN
                            has_next_page := true;
                        END IF;
                    END IF;
                END LOOP;
            END IF;

            IF has_link_header THEN
                IF has_next_page THEN
                    page := page + 1;
                    CONTINUE repo_page_loop;
                ELSE
                    EXIT repo_page_loop;
                END IF;
            ELSIF repo_count < per_page THEN
                EXIT repo_page_loop;
            ELSE
                page := page + 1;
            END IF;
        END LOOP;
    END LOOP;
END;
$$;
