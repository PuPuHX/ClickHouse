#!/bin/bash

for folder in */; do
  echo $folder
  cd "$folder"
  file_count=$(find . -maxdepth 1 | wc -l)
  git_count=$(find . -maxdepth 1 -type f -name ".git" | wc -l)
  echo $file_count
  echo $git_count

  if [ ${git_count} -eq 1 ] && [ ${file_count} -eq 2 ]; then
    rm .git
  fi
  cd ..
done