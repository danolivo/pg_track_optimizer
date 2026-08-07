#!/bin/bash
# One-command GCP benchmark run for pg_track_optimizer.
#
#   PROJECT=my-project ./benchmark/gcp-run.sh
#
# Creates a c3d-highcpu-60 spot instance, builds PostgreSQL 18 and the
# extension from BRANCH, runs benchmark/bench-suite.sh, downloads the
# results into ./bench-results/, and deletes the instance.
#
# Requirements: gcloud CLI authenticated with permissions to create/delete
# compute instances in PROJECT.
#
# Overridable environment:
#   PROJECT  (required)          GCP project id
#   ZONE     (us-central1-a)     zone with C3D capacity
#   MACHINE  (c3d-highcpu-60)    machine type
#   NAME     (pgto-bench)        instance name
#   REPO     (origin URL)        git URL the VM clones
#   BRANCH   (current branch)    branch the VM benchmarks
set -e

: "${PROJECT:?set PROJECT to your GCP project id}"
ZONE=${ZONE:-us-central1-a}
MACHINE=${MACHINE:-c3d-highcpu-60}
NAME=${NAME:-pgto-bench}
REPO=${REPO:-$(git remote get-url origin)}
BRANCH=${BRANCH:-$(git rev-parse --abbrev-ref HEAD)}
SCRIPTDIR=$(cd "$(dirname "$0")" && pwd)

echo ">> creating $MACHINE spot instance '$NAME' in $ZONE ($REPO @ $BRANCH)"
gcloud compute instances create "$NAME" \
    --project="$PROJECT" --zone="$ZONE" \
    --machine-type="$MACHINE" \
    --provisioning-model=SPOT --instance-termination-action=DELETE \
    --image-family=ubuntu-2404-lts-amd64 --image-project=ubuntu-os-cloud \
    --boot-disk-size=100GB --boot-disk-type=pd-balanced \
    --metadata=bench-repo="$REPO",bench-branch="$BRANCH" \
    --metadata-from-file=startup-script="$SCRIPTDIR/gcp-startup.sh"

cleanup() {
    echo ">> deleting instance '$NAME'"
    gcloud compute instances delete "$NAME" \
        --project="$PROJECT" --zone="$ZONE" --quiet || true
}
trap cleanup EXIT

echo ">> waiting for the suite to finish (build ~10 min + run ~2-3 h)"
echo ">> follow along: gcloud compute ssh $NAME --project=$PROJECT --zone=$ZONE" \
     "-- tail -f /var/tmp/pgto-startup.log"
while true; do
    sleep 120
    if gcloud compute ssh "$NAME" --project="$PROJECT" --zone="$ZONE" \
        --command='test -f /var/tmp/pgto-bench/DONE' 2>/dev/null; then
        break
    fi
done

echo ">> downloading results"
mkdir -p bench-results
for f in results.csv summary.txt pg_test_timing.txt pg_config.txt \
         extension_commit.txt nproc.txt server.log; do
    gcloud compute scp --project="$PROJECT" --zone="$ZONE" \
        "$NAME:/var/tmp/pgto-bench/$f" bench-results/ 2>/dev/null || true
done

echo ">> summary:"
cat bench-results/summary.txt || true
