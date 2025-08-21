{-# LANGUAGE OverloadedStrings #-}

import System.Environment (getArgs)
import System.Process (readProcess)
import Control.Monad (mapM_)
import Data.Char (isSpace)
import Data.Map (Map)
import qualified Data.Map as Map
import Data.Maybe (fromMaybe)

statusMap :: Map String String
statusMap = Map.fromList
  [ ("success", "✅")
  , ("failure", "❌")
  , ("timed_out", "⌛")
  , ("cancelled", "🛑")
  , ("skipped", "⏭️")
  , ("in_progress", "🔁")
  , ("action_required", "⛔")
  , ("neutral", "⭕")
  , ("stale", "🥖")
  , ("queued", "📋")
  , ("loading", "🌀")
  ]

strip :: String -> String
strip = reverse . dropWhile isSpace . reverse . dropWhile isSpace

jqExpr :: String
jqExpr = ".workflow_runs[0].conclusion // .workflow_runs[0].status // \"loading\""

main :: IO ()
main = do
  users <- getArgs
  if null users
     then putStrLn "usage: ghstatus-hs <user> [user2 ...]"
     else do
       repos <- concat <$> mapM fetchRepos users
       mapM_ printStatus repos

fetchRepos :: String -> IO [String]
fetchRepos u = do
  out <- readProcess "gh" ["repo","list",u,"--public","--limit","500","--json","nameWithOwner","--jq",".[].nameWithOwner"] ""
  pure (lines out)

fetchStatus :: String -> IO String
fetchStatus repo = do
  out <- readProcess "gh" ["api","repos/" ++ repo ++ "/actions/runs?per_page=1","--jq",jqExpr] ""
  pure (strip out)

printStatus :: String -> IO ()
printStatus repo = do
  st <- fetchStatus repo
  putStrLn (iconFor st ++ " " ++ repo)

iconFor :: String -> String
iconFor st = fromMaybe "➖" (Map.lookup st statusMap)
