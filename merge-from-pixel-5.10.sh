#! /bin/sh -u
# SPDX-License-Identifier: Apache-2.0
#
# (c) 2020,2023, Google

# set BUG=xxx in the environment to autoamtically add a
#   Bug: xxx
# tag to each (merge) commit message
progname="${0##*/}"

[ "USAGE: repo_sync_rebase_prune

Perform an aggresive repo sync and cleanup" ]
repo_sync_rebase_prune() {
  if [ ! -d .repo ]; then
    echo "Not at top of repo" >&2
    return 1
  fi
  repo sync -j12 || true
  repo rebase || return 1
  repo sync -j12 2>&1 |
    tee /dev/stderr |
    grep '^error: \|^Failing repos:\|^Please commit your changes or stash them' >/dev/null &&
    exit 1
  repo rebase || return 1
  repo prune || return 1
  return 0
}

#repo_sync_rebase_prune || exit

[ "USAGE: do_merge <directory> <repo> <branch>

Perform a merge over a repo, using BRANCH as the holder." ]

BRANCH="merge.`date +%F`"
AUTHOR_NAME="`git config user.name`"
AUTHOR_EMAIL="`git config user.email`"

do_merge() {
  local orev

  dir=${1}
  shift
  from_repo=${1}
  shift
  from_branch=${1}
  shift
  (
    cd ${dir} || exit 1
    #branch=`git branch -r 2>&1 | sed -n 's/ *m\/.* -> //p'`
    #[ -n "${branch}" ] || branch=partner/android-gs-pixel-mainline
    branch=partner/android14-gs-pixel-6.1
    repo start ${BRANCH} . || exit 1
    commits="`git cherry -v ${branch} ${from_repo}/${from_branch} |
                sed -n 's/^[+] //p'`"
    titles="`echo \"${commits}\" |
               sed 's/[0-9a-fA-F]* /  /'`"
    bug="`echo \"${commits}\" |
            while read sha title; do
              git show --no-patch ${sha}
            done |
            sed -n 's/ *Bug: //p' |
            tr ' \t,' '\n\n\n' |
            sed 's@b/\([0-9]\)@\1@g' |
            sort -u |
            grep -v '^$' |
            sed 's/.*/Bug: &/'`"

    orev=$(git rev-parse HEAD)
    git fetch ${from_repo} ${from_branch} && \
    git merge --no-ff --commit --signoff --log=100 ${from_repo}/${from_branch} --m "Merge ${from_repo}/${from_branch} into ${branch}
${@}
" || exit 1
    # add the Bug: tag only if the merge wasn't a noop
    if [ "${orev}" != "$(git rev-parse HEAD)" ] \
        && [ -n "${BUG:-}" ] ; then
      git show -s --format=%B HEAD \
      | git -c trailer.where=before interpret-trailers --trailer "Bug: ${BUG}" \
      | git commit --amend -F - || exit 1
    fi
    echo
  ) ||
    echo Failed merge of ${dir} >&2
}

[ "Perform Merge" ]

find private/google-modules -name .git |
  while read gitdir; do
    dir=${gitdir%/.git}
    case ${dir} in
      */soc/gs)
        # Note: this project doesn't have an android13 upstream branch
        ;;
      *)
        do_merge ${dir} partner android13-gs-pixel-5.10-udc
        ;;
    esac ||
      echo ERROR: merge ${dir} failed
  done 2>&1 |
  tee /dev/stderr |
  grep 'Failed merge of ' |
  (
    OUT=
    while read o; do
      OUT="${OUT}
${o}"
    done
    repo prune
    echo "${OUT}" >&2
  )
